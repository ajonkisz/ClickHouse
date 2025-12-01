# ClickHouse TimeSeries Compression Improvements

## Executive Summary

This document outlines concrete improvements to ClickHouse's compression codecs to achieve storage efficiency comparable to VictoriaMetrics (0.4-1.5 bytes/sample vs current 2.3 bytes/sample).

### Current State vs Target

| Metric | ClickHouse Current | VictoriaMetrics | Target |
|--------|-------------------|-----------------|--------|
| **Bytes/sample** | 2.31 | 0.4-1.5 | < 1.0 |
| **Timestamp encoding** | 1.67 bytes | 0.1-0.3 bytes | 0.25 bytes |
| **Value encoding** | 0.51 bytes | 0.2-0.5 bytes | 0.35 bytes |
| **Series ID** | 0.12 bytes | 0.0-0.2 bytes | 0.05 bytes |

### Bytes Breakdown (Current)

```
┌─────────────────┬──────────────┬──────────────┬───────────────────────────────┐
│ Column          │ Raw (bytes)  │ Compressed   │ % of Total                    │
├─────────────────┼──────────────┼──────────────┼───────────────────────────────┤
│ timestamp       │     8.00     │    1.67      │ 72.4%  ← PRIORITY 1           │
│ value           │     8.00     │    0.51      │ 22.0%  ← PRIORITY 2           │
│ id (UUID)       │    16.00     │    0.12      │  5.4%  ← PRIORITY 3           │
├─────────────────┼──────────────┼──────────────┼───────────────────────────────┤
│ TOTAL           │    32.00     │    2.31      │ 100%                          │
└─────────────────┴──────────────┴──────────────┴───────────────────────────────┘
```

---

## Priority 1: DoubleDeltaVarInt Codec

### Impact: 1.67 → 0.25 bytes/timestamp (85% reduction)

### Problem Statement

ClickHouse's current `DoubleDelta` codec computes delta-of-delta correctly, but stores values using fixed-width integers (8 bytes). This negates most compression benefits because:

1. For regular scrape intervals (e.g., 15 seconds), delta-of-delta is typically 0
2. A zero stored as 8 bytes + ZSTD still takes ~1-2 bytes
3. VictoriaMetrics stores zero as 1 bit

**Proof from testing:**
```
Delta(8), ZSTD(1):       1.71 bytes/row  ← Current best
DoubleDelta, ZSTD(1):    2.66 bytes/row  ← WORSE due to fixed-width storage
```

### Why This Matters

- Timestamps consume **72.4%** of compressed storage
- With proper varint encoding, this drops to ~10%
- Single biggest improvement possible

### Implementation Details

#### New Codec Registration

**File:** `src/Compression/CompressionFactory.cpp`

```cpp
// Register new codec (backwards compatible - doesn't modify existing DoubleDelta)
void registerCodecDoubleDeltaVarInt(CompressionCodecFactory & factory);

// In registerCodecs():
registerCodecDoubleDeltaVarInt(factory);
```

#### New Codec Header

**File:** `src/Compression/CompressionCodecDoubleDeltaVarInt.h`

```cpp
#pragma once

#include <Compression/ICompressionCodec.h>

namespace DB
{

/** 
 * DoubleDeltaVarInt codec - optimized for time-series timestamps
 * 
 * Computes delta-of-delta (same as DoubleDelta), but stores using
 * variable-length integer encoding inspired by VictoriaMetrics.
 * 
 * Encoding scheme:
 *   0xxxxxxx                    - value in [-63, 64], 1 byte
 *   10xxxxxx xxxxxxxx           - value in [-8191, 8192], 2 bytes
 *   110xxxxx xxxxxxxx xxxxxxxx  - value in [-1048575, 1048576], 3 bytes
 *   1110xxxx + 4 bytes          - value in [-2^31, 2^31], 5 bytes
 *   1111xxxx + 8 bytes          - full 64-bit value, 9 bytes
 *   
 * For regular Prometheus scrapes (15s interval), most values are 0,
 * which encodes as a single byte (0x40 = 64 + 0 = zero delta-of-delta).
 */
class CompressionCodecDoubleDeltaVarInt : public ICompressionCodec
{
public:
    explicit CompressionCodecDoubleDeltaVarInt(UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;
    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    void doDecompressData(const char * source, UInt32 source_size, 
                          char * dest, UInt32 uncompressed_size) const override;
    
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;
    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }

private:
    UInt8 data_bytes_size;
};

}
```

#### Core Implementation

**File:** `src/Compression/CompressionCodecDoubleDeltaVarInt.cpp`

```cpp
#include <Compression/CompressionCodecDoubleDeltaVarInt.h>
#include <Compression/CompressionFactory.h>
#include <Common/BitHelpers.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_CODEC_PARAMETER;
}

// Method byte must be unique across all codecs
static constexpr UInt8 CODEC_METHOD_BYTE = 0x97;  // Check for conflicts!

CompressionCodecDoubleDeltaVarInt::CompressionCodecDoubleDeltaVarInt(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    if (data_bytes_size != 1 && data_bytes_size != 2 && 
        data_bytes_size != 4 && data_bytes_size != 8)
        throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
            "DoubleDeltaVarInt codec: data type size must be 1, 2, 4, or 8 bytes");
}

uint8_t CompressionCodecDoubleDeltaVarInt::getMethodByte() const
{
    return CODEC_METHOD_BYTE;
}

//------------------------------------------------------------------------------
// Variable-length integer encoding/decoding
//------------------------------------------------------------------------------

namespace
{

/// Encode a signed 64-bit integer using variable-length encoding
/// Returns number of bytes written
inline size_t encodeVarInt64(Int64 value, UInt8 * dest)
{
    // Bias value to make it unsigned for easier encoding
    // Range [-63, 64] -> [0, 127] -> 1 byte
    if (value >= -63 && value <= 64)
    {
        dest[0] = static_cast<UInt8>(value + 63);  // 0x00-0x7F
        return 1;
    }
    
    // Range [-8191, 8192] -> 2 bytes
    if (value >= -8191 && value <= 8192)
    {
        UInt16 encoded = static_cast<UInt16>(value + 8191);
        dest[0] = 0x80 | static_cast<UInt8>(encoded >> 8);  // 10xxxxxx
        dest[1] = static_cast<UInt8>(encoded & 0xFF);
        return 2;
    }
    
    // Range [-1048575, 1048576] -> 3 bytes
    if (value >= -1048575 && value <= 1048576)
    {
        UInt32 encoded = static_cast<UInt32>(value + 1048575);
        dest[0] = 0xC0 | static_cast<UInt8>(encoded >> 16);  // 110xxxxx
        dest[1] = static_cast<UInt8>((encoded >> 8) & 0xFF);
        dest[2] = static_cast<UInt8>(encoded & 0xFF);
        return 3;
    }
    
    // Range [-2^31, 2^31] -> 5 bytes
    if (value >= INT32_MIN && value <= INT32_MAX)
    {
        dest[0] = 0xE0;  // 1110xxxx (we use xxxx = 0)
        Int32 narrow = static_cast<Int32>(value);
        memcpy(dest + 1, &narrow, 4);
        return 5;
    }
    
    // Full 64-bit value -> 9 bytes
    dest[0] = 0xF0;  // 1111xxxx
    memcpy(dest + 1, &value, 8);
    return 9;
}

/// Decode a variable-length integer
/// Returns number of bytes consumed
inline size_t decodeVarInt64(const UInt8 * src, Int64 & value)
{
    UInt8 header = src[0];
    
    if ((header & 0x80) == 0)  // 0xxxxxxx
    {
        value = static_cast<Int64>(header) - 63;
        return 1;
    }
    
    if ((header & 0xC0) == 0x80)  // 10xxxxxx
    {
        UInt16 encoded = (static_cast<UInt16>(header & 0x3F) << 8) | src[1];
        value = static_cast<Int64>(encoded) - 8191;
        return 2;
    }
    
    if ((header & 0xE0) == 0xC0)  // 110xxxxx
    {
        UInt32 encoded = (static_cast<UInt32>(header & 0x1F) << 16) |
                         (static_cast<UInt32>(src[1]) << 8) |
                         src[2];
        value = static_cast<Int64>(encoded) - 1048575;
        return 3;
    }
    
    if ((header & 0xF0) == 0xE0)  // 1110xxxx
    {
        Int32 narrow;
        memcpy(&narrow, src + 1, 4);
        value = narrow;
        return 5;
    }
    
    // 1111xxxx - full 64-bit
    memcpy(&value, src + 1, 8);
    return 9;
}

} // anonymous namespace

//------------------------------------------------------------------------------
// Compression
//------------------------------------------------------------------------------

template <typename ValueType>
UInt32 compressDataDoubleDeltaVarInt(const char * source, UInt32 source_size, char * dest)
{
    static_assert(std::is_integral_v<ValueType> || std::is_same_v<ValueType, Float32> || 
                  std::is_same_v<ValueType, Float64>,
                  "ValueType must be integral or floating point");
    
    const size_t count = source_size / sizeof(ValueType);
    if (count < 2)
    {
        memcpy(dest, source, source_size);
        return source_size;
    }
    
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);
    
    // Store header: data type size and count
    *out++ = sizeof(ValueType);
    memcpy(out, &count, sizeof(UInt32));
    out += sizeof(UInt32);
    
    // Store first two values uncompressed (needed for delta computation)
    const ValueType * in = reinterpret_cast<const ValueType *>(source);
    memcpy(out, &in[0], sizeof(ValueType));
    out += sizeof(ValueType);
    memcpy(out, &in[1], sizeof(ValueType));
    out += sizeof(ValueType);
    
    // Compute and encode delta-of-deltas
    using SignedType = std::make_signed_t<std::conditional_t<std::is_floating_point_v<ValueType>,
                                                              Int64, ValueType>>;
    
    ValueType prev = in[1];
    SignedType prev_delta = static_cast<SignedType>(in[1]) - static_cast<SignedType>(in[0]);
    
    for (size_t i = 2; i < count; ++i)
    {
        ValueType curr = in[i];
        SignedType delta = static_cast<SignedType>(curr) - static_cast<SignedType>(prev);
        Int64 double_delta = static_cast<Int64>(delta) - static_cast<Int64>(prev_delta);
        
        out += encodeVarInt64(double_delta, out);
        
        prev = curr;
        prev_delta = delta;
    }
    
    return static_cast<UInt32>(out - reinterpret_cast<UInt8 *>(dest));
}

UInt32 CompressionCodecDoubleDeltaVarInt::doCompressData(
    const char * source, UInt32 source_size, char * dest) const
{
    switch (data_bytes_size)
    {
        case 1: return compressDataDoubleDeltaVarInt<UInt8>(source, source_size, dest);
        case 2: return compressDataDoubleDeltaVarInt<UInt16>(source, source_size, dest);
        case 4: return compressDataDoubleDeltaVarInt<UInt32>(source, source_size, dest);
        case 8: return compressDataDoubleDeltaVarInt<UInt64>(source, source_size, dest);
        default:
            throw Exception(ErrorCodes::CANNOT_COMPRESS, 
                "Unsupported data size for DoubleDeltaVarInt");
    }
}

//------------------------------------------------------------------------------
// Decompression
//------------------------------------------------------------------------------

template <typename ValueType>
void decompressDataDoubleDeltaVarInt(const char * source, UInt32 source_size, 
                                      char * dest, UInt32 uncompressed_size)
{
    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    
    // Read header
    UInt8 stored_data_size = *in++;
    if (stored_data_size != sizeof(ValueType))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, 
            "DoubleDeltaVarInt: data size mismatch");
    
    UInt32 count;
    memcpy(&count, in, sizeof(UInt32));
    in += sizeof(UInt32);
    
    if (count < 2)
    {
        memcpy(dest, in, uncompressed_size);
        return;
    }
    
    ValueType * out = reinterpret_cast<ValueType *>(dest);
    
    // Read first two values
    memcpy(&out[0], in, sizeof(ValueType));
    in += sizeof(ValueType);
    memcpy(&out[1], in, sizeof(ValueType));
    in += sizeof(ValueType);
    
    // Decode delta-of-deltas
    using SignedType = std::make_signed_t<std::conditional_t<std::is_floating_point_v<ValueType>,
                                                              Int64, ValueType>>;
    
    ValueType prev = out[1];
    SignedType prev_delta = static_cast<SignedType>(out[1]) - static_cast<SignedType>(out[0]);
    
    for (size_t i = 2; i < count; ++i)
    {
        Int64 double_delta;
        in += decodeVarInt64(in, double_delta);
        
        SignedType delta = prev_delta + static_cast<SignedType>(double_delta);
        ValueType curr = static_cast<ValueType>(static_cast<SignedType>(prev) + delta);
        
        out[i] = curr;
        prev = curr;
        prev_delta = delta;
    }
}

void CompressionCodecDoubleDeltaVarInt::doDecompressData(
    const char * source, UInt32 source_size, 
    char * dest, UInt32 uncompressed_size) const
{
    switch (data_bytes_size)
    {
        case 1: decompressDataDoubleDeltaVarInt<UInt8>(source, source_size, dest, uncompressed_size); break;
        case 2: decompressDataDoubleDeltaVarInt<UInt16>(source, source_size, dest, uncompressed_size); break;
        case 4: decompressDataDoubleDeltaVarInt<UInt32>(source, source_size, dest, uncompressed_size); break;
        case 8: decompressDataDoubleDeltaVarInt<UInt64>(source, source_size, dest, uncompressed_size); break;
        default:
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, 
                "Unsupported data size for DoubleDeltaVarInt");
    }
}

UInt32 CompressionCodecDoubleDeltaVarInt::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Header: 1 (data size) + 4 (count) + 2 * data_bytes_size (first two values)
    // Each remaining value: max 9 bytes
    const size_t count = uncompressed_size / data_bytes_size;
    return 1 + 4 + 2 * data_bytes_size + (count > 2 ? (count - 2) * 9 : 0);
}

void CompressionCodecDoubleDeltaVarInt::updateHash(SipHash & hash) const
{
    hash.update(CODEC_METHOD_BYTE);
    hash.update(data_bytes_size);
}

//------------------------------------------------------------------------------
// Registration
//------------------------------------------------------------------------------

void registerCodecDoubleDeltaVarInt(CompressionCodecFactory & factory)
{
    factory.registerCompressionCodec(
        "DoubleDeltaVarInt",
        CODEC_METHOD_BYTE,
        [&](const ASTPtr & arguments) -> CompressionCodecPtr
        {
            UInt8 data_bytes_size = 8;  // Default to 8 bytes (for DateTime64/Int64)
            
            if (arguments && !arguments->children.empty())
            {
                const auto * literal = arguments->children[0]->as<ASTLiteral>();
                if (literal)
                    data_bytes_size = static_cast<UInt8>(literal->value.safeGet<UInt64>());
            }
            
            return std::make_shared<CompressionCodecDoubleDeltaVarInt>(data_bytes_size);
        });
}

} // namespace DB
```

### Testing Strategy

#### Unit Tests

**File:** `src/Compression/tests/gtest_compressionCodecDoubleDeltaVarInt.cpp`

```cpp
#include <gtest/gtest.h>
#include <Compression/CompressionCodecDoubleDeltaVarInt.h>

TEST(DoubleDeltaVarInt, RegularIntervals)
{
    // Simulate 15-second Prometheus scrapes
    std::vector<Int64> timestamps;
    Int64 base = 1700000000000;  // milliseconds
    for (int i = 0; i < 1000; ++i)
        timestamps.push_back(base + i * 15000);  // 15 second intervals
    
    // Compress
    CompressionCodecDoubleDeltaVarInt codec(8);
    std::vector<char> compressed(codec.getMaxCompressedDataSize(timestamps.size() * 8));
    UInt32 compressed_size = codec.doCompressData(
        reinterpret_cast<const char*>(timestamps.data()),
        timestamps.size() * 8,
        compressed.data());
    
    // Should achieve < 0.3 bytes per timestamp for regular intervals
    double bytes_per_value = static_cast<double>(compressed_size) / timestamps.size();
    EXPECT_LT(bytes_per_value, 0.3);
    
    // Decompress and verify
    std::vector<Int64> decompressed(timestamps.size());
    codec.doDecompressData(
        compressed.data(), compressed_size,
        reinterpret_cast<char*>(decompressed.data()),
        timestamps.size() * 8);
    
    EXPECT_EQ(timestamps, decompressed);
}

TEST(DoubleDeltaVarInt, VariableIntervals)
{
    // Variable intervals (still should be better than fixed-width)
    std::vector<Int64> timestamps = {1000, 1015, 1028, 1045, 1058, 1073};
    
    CompressionCodecDoubleDeltaVarInt codec(8);
    std::vector<char> compressed(codec.getMaxCompressedDataSize(timestamps.size() * 8));
    UInt32 compressed_size = codec.doCompressData(
        reinterpret_cast<const char*>(timestamps.data()),
        timestamps.size() * 8,
        compressed.data());
    
    // Should still be < 3 bytes per timestamp
    double bytes_per_value = static_cast<double>(compressed_size) / timestamps.size();
    EXPECT_LT(bytes_per_value, 3.0);
}

TEST(DoubleDeltaVarInt, EdgeCases)
{
    // Test empty, single value, two values
    CompressionCodecDoubleDeltaVarInt codec(8);
    
    // Single value
    Int64 single = 1000;
    std::vector<char> compressed(64);
    UInt32 size = codec.doCompressData(
        reinterpret_cast<const char*>(&single), 8, compressed.data());
    EXPECT_EQ(size, 8);  // Just stored as-is
}
```

#### Integration Tests

```sql
-- Test with real TimeSeries data
CREATE TABLE test_compression_varint
(
    ts DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(1))
) ENGINE = MergeTree ORDER BY ts;

-- Insert regular interval data
INSERT INTO test_compression_varint
SELECT toDateTime64('2024-01-01 00:00:00', 3) + INTERVAL number * 15 SECOND
FROM numbers(1000000);

-- Check compression ratio
SELECT 
    formatReadableSize(sum(column_data_compressed_bytes)) as compressed,
    formatReadableSize(sum(column_data_uncompressed_bytes)) as uncompressed,
    round(sum(column_data_uncompressed_bytes) / sum(column_data_compressed_bytes), 2) as ratio
FROM system.parts_columns
WHERE table = 'test_compression_varint' AND column = 'ts';

-- Expected: ratio > 30x for regular intervals
```

### Backwards Compatibility

- ✅ New codec name `DoubleDeltaVarInt` - doesn't conflict with `DoubleDelta`
- ✅ Unique method byte (`0x97`) - doesn't conflict with existing codecs
- ✅ Existing data using `DoubleDelta` continues to work unchanged
- ✅ Users explicitly opt-in by specifying `CODEC(DoubleDeltaVarInt, ZSTD(1))`

---

## Priority 2: Enhanced Gorilla (GorillaV2)

### Impact: 0.51 → 0.30 bytes/value (41% reduction)

### Problem Statement

ClickHouse's Gorilla codec implements Facebook's paper but misses optimization opportunities:

1. Doesn't track leading/trailing zero patterns across XOR values
2. Stores full control bits even when pattern is predictable
3. Doesn't use run-length encoding for repeated values

### Why This Matters

- Value column is **22%** of compressed storage
- Time-series often have repeated or slowly-changing values
- VictoriaMetrics achieves 0.2-0.4 bytes with enhanced Gorilla

### Implementation Details

#### New Codec Header

**File:** `src/Compression/CompressionCodecGorillaV2.h`

```cpp
#pragma once

#include <Compression/ICompressionCodec.h>

namespace DB
{

/**
 * GorillaV2 - Enhanced Gorilla codec with leading/trailing zero optimization
 * 
 * Improvements over standard Gorilla:
 * 1. Tracks leading zeros and trailing zeros from previous XOR
 * 2. If current XOR fits within previous meaningful bits window, use short encoding
 * 3. Special handling for zero XOR (repeated value) - just 1 bit
 * 
 * Encoding scheme:
 *   0             - XOR is zero (repeated value), 1 bit
 *   10 + bits     - XOR fits in previous window, 2 + meaningful_bits
 *   11 + 5 + 6 + bits - New window: 2 + 5 (leading) + 6 (length) + meaningful_bits
 */
class CompressionCodecGorillaV2 : public ICompressionCodec
{
public:
    CompressionCodecGorillaV2();
    
    uint8_t getMethodByte() const override;
    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    void doDecompressData(const char * source, UInt32 source_size, 
                          char * dest, UInt32 uncompressed_size) const override;
    
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;
    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }
};

}
```

#### Core Implementation

**File:** `src/Compression/CompressionCodecGorillaV2.cpp`

```cpp
#include <Compression/CompressionCodecGorillaV2.h>
#include <Compression/CompressionFactory.h>
#include <bit>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
}

static constexpr UInt8 CODEC_METHOD_BYTE = 0x98;

//------------------------------------------------------------------------------
// Bit stream writer/reader for variable bit-length encoding
//------------------------------------------------------------------------------

class BitWriter
{
public:
    BitWriter(UInt8 * dest) : buffer(dest), bit_pos(0) {}
    
    void writeBit(bool bit)
    {
        if (bit)
            buffer[bit_pos / 8] |= (1 << (7 - (bit_pos % 8)));
        else
            buffer[bit_pos / 8] &= ~(1 << (7 - (bit_pos % 8)));
        ++bit_pos;
    }
    
    void writeBits(UInt64 value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
            writeBit((value >> i) & 1);
    }
    
    size_t bytesWritten() const { return (bit_pos + 7) / 8; }
    
private:
    UInt8 * buffer;
    size_t bit_pos;
};

class BitReader
{
public:
    BitReader(const UInt8 * src) : buffer(src), bit_pos(0) {}
    
    bool readBit()
    {
        bool result = (buffer[bit_pos / 8] >> (7 - (bit_pos % 8))) & 1;
        ++bit_pos;
        return result;
    }
    
    UInt64 readBits(int count)
    {
        UInt64 result = 0;
        for (int i = 0; i < count; ++i)
            result = (result << 1) | (readBit() ? 1 : 0);
        return result;
    }
    
private:
    const UInt8 * buffer;
    size_t bit_pos;
};

//------------------------------------------------------------------------------
// Compression
//------------------------------------------------------------------------------

UInt32 CompressionCodecGorillaV2::doCompressData(
    const char * source, UInt32 source_size, char * dest) const
{
    if (source_size % 8 != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS, 
            "GorillaV2: source size must be multiple of 8");
    
    const UInt64 * in = reinterpret_cast<const UInt64 *>(source);
    const size_t count = source_size / 8;
    
    if (count == 0)
        return 0;
    
    // Store count in header
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);
    memcpy(out, &count, sizeof(UInt32));
    out += sizeof(UInt32);
    
    // Store first value uncompressed
    memcpy(out, &in[0], 8);
    out += 8;
    
    if (count == 1)
        return sizeof(UInt32) + 8;
    
    BitWriter writer(out);
    
    UInt64 prev_value = in[0];
    UInt64 prev_xor = 0;
    int prev_leading = 64;
    int prev_trailing = 64;
    int prev_meaningful = 0;
    
    for (size_t i = 1; i < count; ++i)
    {
        UInt64 curr = in[i];
        UInt64 xor_val = curr ^ prev_value;
        
        if (xor_val == 0)
        {
            // Repeated value: just 1 bit
            writer.writeBit(0);
        }
        else
        {
            writer.writeBit(1);
            
            int leading = std::countl_zero(xor_val);
            int trailing = std::countr_zero(xor_val);
            int meaningful = 64 - leading - trailing;
            
            // Check if XOR fits within previous meaningful bits window
            if (leading >= prev_leading && trailing >= prev_trailing && prev_meaningful > 0)
            {
                // Use previous window: 2 + meaningful bits
                writer.writeBit(0);
                UInt64 meaningful_bits = xor_val >> prev_trailing;
                writer.writeBits(meaningful_bits, prev_meaningful);
            }
            else
            {
                // New window: 2 + 5 (leading) + 6 (length) + meaningful bits
                writer.writeBit(1);
                writer.writeBits(leading, 5);  // 0-31 leading zeros
                writer.writeBits(meaningful - 1, 6);  // 1-64 meaningful bits
                UInt64 meaningful_bits = xor_val >> trailing;
                writer.writeBits(meaningful_bits, meaningful);
                
                prev_leading = leading;
                prev_trailing = trailing;
                prev_meaningful = meaningful;
            }
        }
        
        prev_value = curr;
        prev_xor = xor_val;
    }
    
    return sizeof(UInt32) + 8 + writer.bytesWritten();
}

//------------------------------------------------------------------------------
// Decompression
//------------------------------------------------------------------------------

void CompressionCodecGorillaV2::doDecompressData(
    const char * source, UInt32 source_size, 
    char * dest, UInt32 uncompressed_size) const
{
    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    
    UInt32 count;
    memcpy(&count, in, sizeof(UInt32));
    in += sizeof(UInt32);
    
    UInt64 * out = reinterpret_cast<UInt64 *>(dest);
    
    // Read first value
    memcpy(&out[0], in, 8);
    in += 8;
    
    if (count == 1)
        return;
    
    BitReader reader(in);
    
    UInt64 prev_value = out[0];
    int prev_leading = 0;
    int prev_trailing = 0;
    int prev_meaningful = 64;
    
    for (size_t i = 1; i < count; ++i)
    {
        if (!reader.readBit())
        {
            // Zero XOR - repeated value
            out[i] = prev_value;
        }
        else
        {
            UInt64 xor_val;
            
            if (!reader.readBit())
            {
                // Use previous window
                UInt64 meaningful_bits = reader.readBits(prev_meaningful);
                xor_val = meaningful_bits << prev_trailing;
            }
            else
            {
                // New window
                prev_leading = reader.readBits(5);
                prev_meaningful = reader.readBits(6) + 1;
                prev_trailing = 64 - prev_leading - prev_meaningful;
                UInt64 meaningful_bits = reader.readBits(prev_meaningful);
                xor_val = meaningful_bits << prev_trailing;
            }
            
            out[i] = prev_value ^ xor_val;
            prev_value = out[i];
        }
    }
}

UInt32 CompressionCodecGorillaV2::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Worst case: header + first value + 1 byte per value
    return sizeof(UInt32) + uncompressed_size + (uncompressed_size / 8);
}

void CompressionCodecGorillaV2::updateHash(SipHash & hash) const
{
    hash.update(CODEC_METHOD_BYTE);
}

CompressionCodecGorillaV2::CompressionCodecGorillaV2() = default;

uint8_t CompressionCodecGorillaV2::getMethodByte() const
{
    return CODEC_METHOD_BYTE;
}

void registerCodecGorillaV2(CompressionCodecFactory & factory)
{
    factory.registerSimpleCompressionCodec(
        "GorillaV2",
        CODEC_METHOD_BYTE,
        []() { return std::make_shared<CompressionCodecGorillaV2>(); });
}

} // namespace DB
```

### Testing Strategy

```sql
-- Test with metric-like data (slowly changing values)
CREATE TABLE test_gorilla_v2
(
    value Float64 CODEC(GorillaV2, ZSTD(1))
) ENGINE = MergeTree ORDER BY tuple();

-- Insert data with typical metric patterns
INSERT INTO test_gorilla_v2
SELECT 50.0 + sin(number / 100.0) * 10 + rand() % 5 - 2.5
FROM numbers(1000000);

-- Check compression
SELECT 
    formatReadableSize(sum(column_data_compressed_bytes)) as compressed,
    round(sum(column_data_uncompressed_bytes) / sum(column_data_compressed_bytes), 2) as ratio
FROM system.parts_columns
WHERE table = 'test_gorilla_v2' AND column = 'value';

-- Expected: ratio > 20x for typical metric data
```

### Backwards Compatibility

- ✅ New codec name `GorillaV2`
- ✅ Unique method byte (`0x98`)
- ✅ Existing `Gorilla` codec unchanged

---

## Priority 3: Block-Level UUID Deduplication

### Impact: 0.12 → 0.03 bytes/row (75% reduction)

### Problem Statement

Currently each row stores a full UUID linking to the tags table. With 156x compression, this is already quite efficient, but we can do better by:

1. Storing unique UUIDs once per block in a dictionary
2. Replacing per-row UUID with a short dictionary index

### Why This Matters

- Already small overhead (0.12 bytes/row)
- But at scale, every bit counts
- Enables future optimizations like block-level deduplication

### Implementation Approach

This requires changes to the MergeTree storage engine, not just a codec. It's more invasive.

**File to modify:** `src/Storages/MergeTree/MergeTreeDataPartWriterWide.cpp`

The idea is to detect when a column has high repetition within a block and automatically apply dictionary encoding.

### Testing Strategy

This is more complex to test and requires integration tests with the full storage layer.

### Backwards Compatibility

- Would need to be a new feature flag or table setting
- Existing parts continue to work unchanged
- Only newly written parts use the optimization

---

## Priority 4: Complete PromQL Support

### Impact: User experience, not storage

### Problem Statement

ClickHouse's PromQL implementation is incomplete:

```promql
avg(cpu_usage_percent)     -- ❌ "AggregationOperator not implemented"
sum by (host)(metric)      -- ❌ Grouping not supported
rate(metric[5m])           -- ❌ Range vectors not implemented
```

### Why This Matters

- Breaks Grafana dashboards
- Forces users to use SQL instead of PromQL
- Limits adoption of ClickHouse for metrics

### Files to Modify

```
src/Processors/Formats/Impl/PrometheusTextOutputFormat.cpp
src/Storages/TimeSeries/PrometheusQueryHandler.cpp (if exists)
```

### Testing Strategy

Use the Prometheus conformance test suite.

---

## Summary: Implementation Priority

| Priority | Improvement | Impact (bytes/row) | Effort | Risk |
|----------|-------------|-------------------|--------|------|
| **1** | DoubleDeltaVarInt | 1.67 → 0.25 (-85%) | Medium | Low |
| **2** | GorillaV2 | 0.51 → 0.30 (-41%) | Medium | Low |
| **3** | Block-Level UUID | 0.12 → 0.03 (-75%) | High | Medium |
| **4** | PromQL Completeness | UX improvement | High | Low |

### Expected Final Result

| State | Bytes/Row | vs VictoriaMetrics |
|-------|-----------|-------------------|
| Current | 2.31 | 3.8x worse |
| After P1 | 0.89 | 1.5x worse |
| After P1+P2 | 0.59 | Competitive |
| After P1+P2+P3 | 0.50 | **On par** |

---

## Appendix: File Locations

```
clickhouse/src/Compression/
├── CompressionCodecDoubleDelta.cpp       # Reference for delta computation
├── CompressionCodecDoubleDelta.h
├── CompressionCodecGorilla.cpp           # Reference for XOR compression
├── CompressionCodecGorilla.h
├── CompressionCodecDoubleDeltaVarInt.cpp # NEW - Priority 1
├── CompressionCodecDoubleDeltaVarInt.h   # NEW - Priority 1
├── CompressionCodecGorillaV2.cpp         # NEW - Priority 2
├── CompressionCodecGorillaV2.h           # NEW - Priority 2
├── CompressionFactory.cpp                # Register new codecs
└── tests/
    ├── gtest_compressionCodecDoubleDeltaVarInt.cpp  # NEW
    └── gtest_compressionCodecGorillaV2.cpp          # NEW
```

---

## References

- [VictoriaMetrics Encoding](https://github.com/VictoriaMetrics/VictoriaMetrics/blob/master/lib/encoding/encoding.go)
- [Facebook Gorilla Paper](https://www.vldb.org/pvldb/vol8/p1816-teller.pdf)
- [ClickHouse Compression Codecs](https://clickhouse.com/docs/en/sql-reference/statements/create/table#column_compression_codec)
- [ClickHouse DoubleDelta Source](https://github.com/ClickHouse/ClickHouse/blob/master/src/Compression/CompressionCodecDoubleDelta.cpp)

