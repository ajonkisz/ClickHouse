#pragma clang diagnostic ignored "-Wreserved-identifier"

#include <Common/SipHash.h>
#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <base/unaligned.h>

#include <Parsers/IAST_fwd.h>
#include <Parsers/ASTLiteral.h>

#include <IO/WriteHelpers.h>

#include <DataTypes/DataTypesNumber.h>

#include <cstring>
#include <type_traits>
#include <limits>
#include <bit>


namespace DB
{

/** BlockGorilla codec - Block-level bit-packed XOR encoding for floats
 *
 * Based on Facebook Gorilla paper with true bit-level encoding.
 *
 * Block format:
 *   [4 bytes: count]
 *   [8 bytes: first value]
 *   [variable: bit-packed XOR values]
 *
 * Bit encoding for XOR values:
 *   - XOR = 0 (same value):      1 bit  (0)
 *   - Same leading/trailing:     2 bits (10) + meaningful bits
 *   - New pattern:               2 + 6 + 6 bits (11 + leading + trailing) + meaningful bits
 *
 * For slowly-changing metrics, most XORs have few bits set,
 * leading to very compact encoding.
 */
class CompressionCodecBlockGorilla : public ICompressionCodec
{
public:
    explicit CompressionCodecBlockGorilla(UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;

    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }
    bool isDeltaCompression() const override { return false; }

    String getDescription() const override
    {
        return "Block-level bit-packed XOR encoding; optimal for slowly-changing floating-point values.";
    }

private:
    UInt8 data_bytes_size;
};


namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
    extern const int BAD_ARGUMENTS;
}

namespace
{

//------------------------------------------------------------------------------
// BitWriter - writes bits to a byte buffer
//------------------------------------------------------------------------------
class BitWriter
{
public:
    explicit BitWriter(UInt8 * buffer) : buffer_(buffer), byte_pos_(0), bit_pos_(0), current_byte_(0) {}

    void writeBit(bool bit)
    {
        if (bit)
            current_byte_ |= (1 << (7 - bit_pos_));
        
        bit_pos_++;
        if (bit_pos_ == 8)
        {
            buffer_[byte_pos_++] = current_byte_;
            current_byte_ = 0;
            bit_pos_ = 0;
        }
    }

    void writeBits(UInt64 value, size_t num_bits)
    {
        for (size_t i = 0; i < num_bits; ++i)
        {
            bool bit = (value >> (num_bits - 1 - i)) & 1;
            writeBit(bit);
        }
    }

    size_t finish()
    {
        if (bit_pos_ > 0)
        {
            buffer_[byte_pos_++] = current_byte_;
        }
        return byte_pos_;
    }

private:
    UInt8 * buffer_;
    size_t byte_pos_;
    size_t bit_pos_;
    UInt8 current_byte_;
};

//------------------------------------------------------------------------------
// BitReader - reads bits from a byte buffer
//------------------------------------------------------------------------------
class BitReader
{
public:
    BitReader(const UInt8 * buffer, size_t size)
        : buffer_(buffer), size_(size), byte_pos_(0), bit_pos_(0) {}

    bool readBit()
    {
        if (byte_pos_ >= size_)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "BlockGorilla: unexpected end of data");
        
        bool bit = (buffer_[byte_pos_] >> (7 - bit_pos_)) & 1;
        bit_pos_++;
        if (bit_pos_ == 8)
        {
            byte_pos_++;
            bit_pos_ = 0;
        }
        return bit;
    }

    UInt64 readBits(size_t num_bits)
    {
        UInt64 value = 0;
        for (size_t i = 0; i < num_bits; ++i)
        {
            value = (value << 1) | (readBit() ? 1 : 0);
        }
        return value;
    }

private:
    const UInt8 * buffer_;
    size_t size_;
    size_t byte_pos_;
    size_t bit_pos_;
};

//------------------------------------------------------------------------------
// Count leading and trailing zeros
//------------------------------------------------------------------------------
inline UInt8 countLeadingZeros64(UInt64 value)
{
    if (value == 0) return 64;
    return static_cast<UInt8>(std::countl_zero(value));
}

inline UInt8 countTrailingZeros64(UInt64 value)
{
    if (value == 0) return 64;
    return static_cast<UInt8>(std::countr_zero(value));
}

inline UInt8 countLeadingZeros32(UInt32 value)
{
    if (value == 0) return 32;
    return static_cast<UInt8>(std::countl_zero(value));
}

inline UInt8 countTrailingZeros32(UInt32 value)
{
    if (value == 0) return 32;
    return static_cast<UInt8>(std::countr_zero(value));
}

//------------------------------------------------------------------------------
// Compression for 64-bit values (Float64)
//------------------------------------------------------------------------------

UInt32 compressData64(const char * source, UInt32 source_size, char * dest)
{
    if (source_size % sizeof(UInt64) != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with BlockGorilla codec, data size {} is not aligned to 8",
                        source_size);

    const UInt32 count = source_size / sizeof(UInt64);
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);

    // Store count in header
    unalignedStoreLittleEndian<UInt32>(out, count);
    out += sizeof(UInt32);

    if (count == 0)
        return sizeof(UInt32);

    const UInt64 * in = reinterpret_cast<const UInt64 *>(source);

    // Store first value uncompressed
    UInt64 first_value = unalignedLoadLittleEndian<UInt64>(in);
    unalignedStoreLittleEndian<UInt64>(out, first_value);
    out += sizeof(UInt64);

    if (count == 1)
        return sizeof(UInt32) + sizeof(UInt64);

    // Bit-pack XOR values
    BitWriter writer(out);
    UInt64 prev_value = first_value;
    UInt8 prev_leading = 0;
    UInt8 prev_trailing = 0;
    bool has_prev_pattern = false;

    for (UInt32 i = 1; i < count; ++i)
    {
        UInt64 curr_value = unalignedLoadLittleEndian<UInt64>(in + i);
        UInt64 xor_val = curr_value ^ prev_value;

        if (xor_val == 0)
        {
            // Same value: 1 bit (0)
            writer.writeBit(false);
        }
        else
        {
            UInt8 leading = countLeadingZeros64(xor_val);
            UInt8 trailing = countTrailingZeros64(xor_val);
            UInt8 meaningful_bits = 64 - leading - trailing;

            // Check if we can reuse previous pattern
            if (has_prev_pattern && leading >= prev_leading && trailing >= prev_trailing)
            {
                // Reuse pattern: 10 + meaningful bits
                writer.writeBit(true);
                writer.writeBit(false);
                
                UInt8 prev_meaningful = 64 - prev_leading - prev_trailing;
                UInt64 meaningful_value = (xor_val >> prev_trailing) & ((1ULL << prev_meaningful) - 1);
                writer.writeBits(meaningful_value, prev_meaningful);
            }
            else
            {
                // New pattern: 11 + 6-bit leading + 6-bit meaningful_bits + meaningful bits
                writer.writeBit(true);
                writer.writeBit(true);
                writer.writeBits(leading, 6);
                writer.writeBits(meaningful_bits, 6);
                
                UInt64 meaningful_value = (xor_val >> trailing) & ((1ULL << meaningful_bits) - 1);
                writer.writeBits(meaningful_value, meaningful_bits);

                prev_leading = leading;
                prev_trailing = trailing;
                has_prev_pattern = true;
            }
        }

        prev_value = curr_value;
    }

    size_t bits_size = writer.finish();
    return static_cast<UInt32>(sizeof(UInt32) + sizeof(UInt64) + bits_size);
}

//------------------------------------------------------------------------------
// Decompression for 64-bit values
//------------------------------------------------------------------------------

void decompressData64(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    const UInt8 * in_end = in + source_size;

    if (source_size < sizeof(UInt32))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: insufficient header data");

    UInt32 count = unalignedLoadLittleEndian<UInt32>(in);
    in += sizeof(UInt32);

    if (count == 0)
        return;

    if (uncompressed_size != count * sizeof(UInt64))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: uncompressed size mismatch");

    UInt64 * out = reinterpret_cast<UInt64 *>(dest);

    // Read first value
    if (in + sizeof(UInt64) > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: unexpected end of data");
    UInt64 first_value = unalignedLoadLittleEndian<UInt64>(in);
    in += sizeof(UInt64);
    unalignedStoreLittleEndian<UInt64>(out++, first_value);

    if (count == 1)
        return;

    // Read bit-packed XOR values
    BitReader reader(in, static_cast<size_t>(in_end - in));
    UInt64 prev_value = first_value;
    UInt8 prev_leading = 0;
    UInt8 prev_meaningful = 0;

    for (UInt32 i = 1; i < count; ++i)
    {
        UInt64 xor_val;

        if (!reader.readBit())
        {
            // 0: same value
            xor_val = 0;
        }
        else if (!reader.readBit())
        {
            // 10: reuse pattern
            UInt64 meaningful_value = reader.readBits(prev_meaningful);
            UInt8 prev_trailing = 64 - prev_leading - prev_meaningful;
            xor_val = meaningful_value << prev_trailing;
        }
        else
        {
            // 11: new pattern
            UInt8 leading = static_cast<UInt8>(reader.readBits(6));
            UInt8 meaningful_bits = static_cast<UInt8>(reader.readBits(6));
            
            if (meaningful_bits == 0)
                meaningful_bits = 64;  // Special case: 0 means 64

            UInt64 meaningful_value = reader.readBits(meaningful_bits);
            UInt8 trailing = 64 - leading - meaningful_bits;
            xor_val = meaningful_value << trailing;

            prev_leading = leading;
            prev_meaningful = meaningful_bits;
        }

        UInt64 curr_value = prev_value ^ xor_val;
        unalignedStoreLittleEndian<UInt64>(out++, curr_value);
        prev_value = curr_value;
    }
}

//------------------------------------------------------------------------------
// Compression for 32-bit values (Float32)
//------------------------------------------------------------------------------

UInt32 compressData32(const char * source, UInt32 source_size, char * dest)
{
    if (source_size % sizeof(UInt32) != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with BlockGorilla codec, data size {} is not aligned to 4",
                        source_size);

    const UInt32 count = source_size / sizeof(UInt32);
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);

    // Store count in header
    unalignedStoreLittleEndian<UInt32>(out, count);
    out += sizeof(UInt32);

    if (count == 0)
        return sizeof(UInt32);

    const UInt32 * in = reinterpret_cast<const UInt32 *>(source);

    // Store first value uncompressed
    UInt32 first_value = unalignedLoadLittleEndian<UInt32>(in);
    unalignedStoreLittleEndian<UInt32>(out, first_value);
    out += sizeof(UInt32);

    if (count == 1)
        return sizeof(UInt32) + sizeof(UInt32);

    // Bit-pack XOR values
    BitWriter writer(out);
    UInt32 prev_value = first_value;
    UInt8 prev_leading = 0;
    UInt8 prev_trailing = 0;
    bool has_prev_pattern = false;

    for (UInt32 i = 1; i < count; ++i)
    {
        UInt32 curr_value = unalignedLoadLittleEndian<UInt32>(in + i);
        UInt32 xor_val = curr_value ^ prev_value;

        if (xor_val == 0)
        {
            writer.writeBit(false);
        }
        else
        {
            UInt8 leading = countLeadingZeros32(xor_val);
            UInt8 trailing = countTrailingZeros32(xor_val);
            UInt8 meaningful_bits = 32 - leading - trailing;

            if (has_prev_pattern && leading >= prev_leading && trailing >= prev_trailing)
            {
                writer.writeBit(true);
                writer.writeBit(false);
                
                UInt8 prev_meaningful = 32 - prev_leading - prev_trailing;
                UInt32 meaningful_value = (xor_val >> prev_trailing) & ((1U << prev_meaningful) - 1);
                writer.writeBits(meaningful_value, prev_meaningful);
            }
            else
            {
                writer.writeBit(true);
                writer.writeBit(true);
                writer.writeBits(leading, 5);  // 5 bits for 0-31
                writer.writeBits(meaningful_bits, 5);
                
                UInt32 meaningful_value = (xor_val >> trailing) & ((1U << meaningful_bits) - 1);
                writer.writeBits(meaningful_value, meaningful_bits);

                prev_leading = leading;
                prev_trailing = trailing;
                has_prev_pattern = true;
            }
        }

        prev_value = curr_value;
    }

    size_t bits_size = writer.finish();
    return static_cast<UInt32>(sizeof(UInt32) + sizeof(UInt32) + bits_size);
}

//------------------------------------------------------------------------------
// Decompression for 32-bit values
//------------------------------------------------------------------------------

void decompressData32(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    const UInt8 * in_end = in + source_size;

    if (source_size < sizeof(UInt32))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: insufficient header data");

    UInt32 count = unalignedLoadLittleEndian<UInt32>(in);
    in += sizeof(UInt32);

    if (count == 0)
        return;

    if (uncompressed_size != count * sizeof(UInt32))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: uncompressed size mismatch");

    UInt32 * out = reinterpret_cast<UInt32 *>(dest);

    // Read first value
    if (in + sizeof(UInt32) > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: unexpected end of data");
    UInt32 first_value = unalignedLoadLittleEndian<UInt32>(in);
    in += sizeof(UInt32);
    unalignedStoreLittleEndian<UInt32>(out++, first_value);

    if (count == 1)
        return;

    BitReader reader(in, static_cast<size_t>(in_end - in));
    UInt32 prev_value = first_value;
    UInt8 prev_leading = 0;
    UInt8 prev_meaningful = 0;

    for (UInt32 i = 1; i < count; ++i)
    {
        UInt32 xor_val;

        if (!reader.readBit())
        {
            xor_val = 0;
        }
        else if (!reader.readBit())
        {
            UInt32 meaningful_value = static_cast<UInt32>(reader.readBits(prev_meaningful));
            UInt8 prev_trailing = 32 - prev_leading - prev_meaningful;
            xor_val = meaningful_value << prev_trailing;
        }
        else
        {
            UInt8 leading = static_cast<UInt8>(reader.readBits(5));
            UInt8 meaningful_bits = static_cast<UInt8>(reader.readBits(5));
            
            if (meaningful_bits == 0)
                meaningful_bits = 32;

            UInt32 meaningful_value = static_cast<UInt32>(reader.readBits(meaningful_bits));
            UInt8 trailing = 32 - leading - meaningful_bits;
            xor_val = meaningful_value << trailing;

            prev_leading = leading;
            prev_meaningful = meaningful_bits;
        }

        UInt32 curr_value = prev_value ^ xor_val;
        unalignedStoreLittleEndian<UInt32>(out++, curr_value);
        prev_value = curr_value;
    }
}

} // anonymous namespace


CompressionCodecBlockGorilla::CompressionCodecBlockGorilla(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    setCodecDescription("BlockGorilla", {std::make_shared<ASTLiteral>(static_cast<UInt64>(data_bytes_size))});
}

uint8_t CompressionCodecBlockGorilla::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::BlockGorilla);
}

void CompressionCodecBlockGorilla::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

UInt32 CompressionCodecBlockGorilla::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    UInt8 bytes_to_skip = source_size % data_bytes_size;
    dest[0] = bytes_to_skip;

    UInt32 output_size = 1;
    if (source_size < data_bytes_size)
    {
        memcpy(dest + output_size, source, source_size);
        output_size += source_size;
    }
    else
    {
        UInt32 source_size_aligned = source_size - bytes_to_skip;
        switch (data_bytes_size)
        {
            case 4:
                output_size += compressData32(source, source_size_aligned, dest + output_size);
                break;
            case 8:
                output_size += compressData64(source, source_size_aligned, dest + output_size);
                break;
            default:
                throw Exception(ErrorCodes::CANNOT_COMPRESS, "Unsupported data size {} for BlockGorilla", static_cast<int>(data_bytes_size));
        }

        if (bytes_to_skip != 0)
        {
            memcpy(dest + output_size, source + source_size_aligned, bytes_to_skip);
            output_size += bytes_to_skip;
        }
    }

    return output_size;
}

void CompressionCodecBlockGorilla::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < 1)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockGorilla: source buffer too small");

    UInt8 bytes_to_skip = source[0];

    if (uncompressed_size < data_bytes_size)
    {
        memcpy(dest, source + 1, uncompressed_size);
    }
    else
    {
        UInt32 source_offset = 1;
        UInt32 uncompressed_size_aligned = uncompressed_size - bytes_to_skip;
        
        switch (data_bytes_size)
        {
            case 4:
                decompressData32(source + source_offset, source_size - source_offset - bytes_to_skip, dest, uncompressed_size_aligned);
                break;
            case 8:
                decompressData64(source + source_offset, source_size - source_offset - bytes_to_skip, dest, uncompressed_size_aligned);
                break;
            default:
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Unsupported data size {} for BlockGorilla", static_cast<int>(data_bytes_size));
        }

        if (bytes_to_skip != 0)
        {
            memcpy(dest + uncompressed_size_aligned, source + source_size - bytes_to_skip, bytes_to_skip);
        }
    }
}

UInt32 CompressionCodecBlockGorilla::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Worst case: header + first value + 2 + 6 + 6 + 64 bits per value
    return 1 + sizeof(UInt32) + data_bytes_size + (uncompressed_size / data_bytes_size) * 10 + 16;
}


void registerCodecBlockGorilla(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::BlockGorilla);
    
    auto reg_func = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        UInt8 data_bytes_size = 8; // Default to 8 bytes (Float64)
        
        if (column_type)
        {
            if (typeid_cast<const DataTypeFloat32 *>(column_type))
                data_bytes_size = 4;
        }
        
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() != 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE,
                    "BlockGorilla codec expects 0 or 1 parameter, got {}", arguments->children.size());
            
            const auto * literal = arguments->children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "BlockGorilla codec parameter must be an unsigned integer");
            
            data_bytes_size = static_cast<UInt8>(literal->value.safeGet<UInt64>());
        }
        
        if (data_bytes_size != 4 && data_bytes_size != 8)
            throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                "BlockGorilla codec parameter must be 4 or 8, got {}", static_cast<int>(data_bytes_size));
        
        return std::make_shared<CompressionCodecBlockGorilla>(data_bytes_size);
    };

    factory.registerCompressionCodecWithType("BlockGorilla", method_code, reg_func);
}

}

