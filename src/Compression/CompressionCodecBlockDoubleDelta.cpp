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
#include <DataTypes/DataTypeDateTime64.h>

#include <cstring>
#include <type_traits>
#include <limits>
#include <vector>


namespace DB
{

/** BlockDoubleDelta codec - Block-level bit-packed delta-of-delta encoding
 *
 * Inspired by Facebook Gorilla paper and VictoriaMetrics.
 * Uses true bit-level encoding for maximum compression.
 *
 * Block format:
 *   [4 bytes: count]
 *   [8 bytes: first timestamp]
 *   [8 bytes: first delta]
 *   [variable: bit-packed delta-of-deltas]
 *
 * Bit encoding for delta-of-delta values:
 *   - dod = 0:               1 bit  (0)
 *   - |dod| in [1, 63]:      8 bits (10 + sign + 6-bit value)
 *   - |dod| in [64, 255]:    12 bits (110 + sign + 8-bit value)
 *   - |dod| in [256, 2047]:  16 bits (1110 + sign + 11-bit value)
 *   - |dod| in [2048, 65535]: 20 bits (11110 + sign + 15-bit value)
 *   - otherwise:             68 bits (11111 + 1 + sign + 64-bit value)
 *
 * For regular scrapes (15s interval), most delta-of-deltas are 0,
 * which encode as 1 BIT (not 1 byte like in DoubleDeltaVarInt).
 */
class CompressionCodecBlockDoubleDelta : public ICompressionCodec
{
public:
    explicit CompressionCodecBlockDoubleDelta(UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;

    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }
    bool isDeltaCompression() const override { return true; }

    String getDescription() const override
    {
        return "Block-level bit-packed delta-of-delta encoding; optimal for regular time-series timestamps.";
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

    [[maybe_unused]] size_t bytesWritten() const { return byte_pos_ + (bit_pos_ > 0 ? 1 : 0); }

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
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "BlockDoubleDelta: unexpected end of data");
        
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

    [[maybe_unused]] bool hasMore() const { return byte_pos_ < size_ || bit_pos_ > 0; }

private:
    const UInt8 * buffer_;
    size_t size_;
    size_t byte_pos_;
    size_t bit_pos_;
};

//------------------------------------------------------------------------------
// Encode delta-of-delta with bit-packing
// Using zigzag encoding for signed values to avoid sign bit complexity
//------------------------------------------------------------------------------
void encodeDeltaOfDelta(BitWriter & writer, Int64 dod)
{
    // Convert to zigzag encoding: (n << 1) ^ (n >> 63)
    // This maps negative numbers to odd positive numbers, positive to even
    UInt64 zigzag = static_cast<UInt64>((dod << 1) ^ (dod >> 63));
    
    if (zigzag == 0)
    {
        // 0: 1 bit
        writer.writeBit(false);
        return;
    }

    if (zigzag <= 127)
    {
        // 10 + 7-bit value = 9 bits
        writer.writeBit(true);
        writer.writeBit(false);
        writer.writeBits(zigzag, 7);
        return;
    }

    if (zigzag <= 16383)
    {
        // 110 + 14-bit value = 17 bits
        writer.writeBit(true);
        writer.writeBit(true);
        writer.writeBit(false);
        writer.writeBits(zigzag, 14);
        return;
    }

    if (zigzag <= 2097151)
    {
        // 1110 + 21-bit value = 25 bits
        writer.writeBit(true);
        writer.writeBit(true);
        writer.writeBit(true);
        writer.writeBit(false);
        writer.writeBits(zigzag, 21);
        return;
    }

    // 1111 + 64-bit value = 68 bits
    writer.writeBit(true);
    writer.writeBit(true);
    writer.writeBit(true);
    writer.writeBit(true);
    writer.writeBits(zigzag, 64);
}

//------------------------------------------------------------------------------
// Decode delta-of-delta with bit-unpacking
//------------------------------------------------------------------------------
Int64 decodeDeltaOfDelta(BitReader & reader)
{
    UInt64 zigzag;
    
    if (!reader.readBit())
    {
        // 0: value is 0
        return 0;
    }

    if (!reader.readBit())
    {
        // 10: 7-bit value
        zigzag = reader.readBits(7);
    }
    else if (!reader.readBit())
    {
        // 110: 14-bit value
        zigzag = reader.readBits(14);
    }
    else if (!reader.readBit())
    {
        // 1110: 21-bit value
        zigzag = reader.readBits(21);
    }
    else
    {
        // 1111: 64-bit value
        zigzag = reader.readBits(64);
    }

    // Convert from zigzag: (zigzag >> 1) ^ -(zigzag & 1)
    return static_cast<Int64>((zigzag >> 1) ^ (~(zigzag & 1) + 1));
}

//------------------------------------------------------------------------------
// Compression
//------------------------------------------------------------------------------

template <typename ValueType>
UInt32 compressDataForType(const char * source, UInt32 source_size, char * dest)
{
    static_assert(std::is_unsigned_v<ValueType>, "ValueType must be unsigned");
    using SignedType = std::make_signed_t<ValueType>;

    if (source_size % sizeof(ValueType) != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with BlockDoubleDelta codec, data size {} is not aligned to {}",
                        source_size, sizeof(ValueType));

    const UInt32 count = source_size / sizeof(ValueType);
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);

    // Store count in header
    unalignedStoreLittleEndian<UInt32>(out, count);
    out += sizeof(UInt32);

    if (count == 0)
        return sizeof(UInt32);

    // Store first value uncompressed
    const ValueType * in = reinterpret_cast<const ValueType *>(source);
    ValueType first_value = unalignedLoadLittleEndian<ValueType>(in);
    unalignedStoreLittleEndian<ValueType>(out, first_value);
    out += sizeof(ValueType);

    if (count == 1)
        return sizeof(UInt32) + sizeof(ValueType);

    // Store first delta uncompressed
    ValueType second_value = unalignedLoadLittleEndian<ValueType>(in + 1);
    SignedType first_delta = static_cast<SignedType>(second_value) - static_cast<SignedType>(first_value);
    unalignedStoreLittleEndian<SignedType>(out, first_delta);
    out += sizeof(SignedType);

    if (count == 2)
        return sizeof(UInt32) + 2 * sizeof(ValueType);

    // Bit-pack delta-of-deltas
    BitWriter writer(out);
    ValueType prev_value = second_value;
    SignedType prev_delta = first_delta;

    for (UInt32 i = 2; i < count; ++i)
    {
        ValueType curr_value = unalignedLoadLittleEndian<ValueType>(in + i);
        SignedType delta = static_cast<SignedType>(curr_value) - static_cast<SignedType>(prev_value);
        Int64 double_delta = static_cast<Int64>(delta) - static_cast<Int64>(prev_delta);

        encodeDeltaOfDelta(writer, double_delta);

        prev_value = curr_value;
        prev_delta = delta;
    }

    size_t bits_size = writer.finish();
    return static_cast<UInt32>(sizeof(UInt32) + 2 * sizeof(ValueType) + bits_size);
}

//------------------------------------------------------------------------------
// Decompression
//------------------------------------------------------------------------------

template <typename ValueType>
void decompressDataForType(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    static_assert(std::is_unsigned_v<ValueType>, "ValueType must be unsigned");
    using SignedType = std::make_signed_t<ValueType>;

    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    const UInt8 * in_end = in + source_size;

    if (source_size < sizeof(UInt32))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockDoubleDelta: insufficient header data");

    // Read count
    UInt32 count = unalignedLoadLittleEndian<UInt32>(in);
    in += sizeof(UInt32);

    if (count == 0)
        return;

    if (uncompressed_size != count * sizeof(ValueType))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockDoubleDelta: uncompressed size mismatch");

    ValueType * out = reinterpret_cast<ValueType *>(dest);

    // Read first value
    if (in + sizeof(ValueType) > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockDoubleDelta: unexpected end of data");
    ValueType first_value = unalignedLoadLittleEndian<ValueType>(in);
    in += sizeof(ValueType);
    unalignedStoreLittleEndian<ValueType>(out++, first_value);

    if (count == 1)
        return;

    // Read first delta
    if (in + sizeof(SignedType) > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockDoubleDelta: unexpected end of data");
    SignedType first_delta = unalignedLoadLittleEndian<SignedType>(in);
    in += sizeof(SignedType);
    ValueType second_value = static_cast<ValueType>(static_cast<SignedType>(first_value) + first_delta);
    unalignedStoreLittleEndian<ValueType>(out++, second_value);

    if (count == 2)
        return;

    // Read bit-packed delta-of-deltas
    BitReader reader(in, static_cast<size_t>(in_end - in));
    ValueType prev_value = second_value;
    SignedType prev_delta = first_delta;

    for (UInt32 i = 2; i < count; ++i)
    {
        Int64 double_delta = decodeDeltaOfDelta(reader);
        SignedType delta = static_cast<SignedType>(static_cast<Int64>(prev_delta) + double_delta);
        ValueType curr_value = static_cast<ValueType>(static_cast<SignedType>(prev_value) + delta);
        
        unalignedStoreLittleEndian<ValueType>(out++, curr_value);
        
        prev_value = curr_value;
        prev_delta = delta;
    }
}

} // anonymous namespace


CompressionCodecBlockDoubleDelta::CompressionCodecBlockDoubleDelta(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    setCodecDescription("BlockDoubleDelta", {std::make_shared<ASTLiteral>(static_cast<UInt64>(data_bytes_size))});
}

uint8_t CompressionCodecBlockDoubleDelta::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::BlockDoubleDelta);
}

void CompressionCodecBlockDoubleDelta::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

UInt32 CompressionCodecBlockDoubleDelta::doCompressData(const char * source, UInt32 source_size, char * dest) const
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
            case 1:
                output_size += compressDataForType<UInt8>(source, source_size_aligned, dest + output_size);
                break;
            case 2:
                output_size += compressDataForType<UInt16>(source, source_size_aligned, dest + output_size);
                break;
            case 4:
                output_size += compressDataForType<UInt32>(source, source_size_aligned, dest + output_size);
                break;
            case 8:
                output_size += compressDataForType<UInt64>(source, source_size_aligned, dest + output_size);
                break;
            default:
                throw Exception(ErrorCodes::CANNOT_COMPRESS, "Unsupported data size {} for BlockDoubleDelta", static_cast<int>(data_bytes_size));
        }

        if (bytes_to_skip != 0)
        {
            memcpy(dest + output_size, source + source_size_aligned, bytes_to_skip);
            output_size += bytes_to_skip;
        }
    }

    return output_size;
}

void CompressionCodecBlockDoubleDelta::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < 1)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress BlockDoubleDelta: source buffer too small");

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
            case 1:
                decompressDataForType<UInt8>(source + source_offset, source_size - source_offset - bytes_to_skip, dest, uncompressed_size_aligned);
                break;
            case 2:
                decompressDataForType<UInt16>(source + source_offset, source_size - source_offset - bytes_to_skip, dest, uncompressed_size_aligned);
                break;
            case 4:
                decompressDataForType<UInt32>(source + source_offset, source_size - source_offset - bytes_to_skip, dest, uncompressed_size_aligned);
                break;
            case 8:
                decompressDataForType<UInt64>(source + source_offset, source_size - source_offset - bytes_to_skip, dest, uncompressed_size_aligned);
                break;
            default:
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Unsupported data size {} for BlockDoubleDelta", static_cast<int>(data_bytes_size));
        }

        if (bytes_to_skip != 0)
        {
            memcpy(dest + uncompressed_size_aligned, source + source_size - bytes_to_skip, bytes_to_skip);
        }
    }
}

UInt32 CompressionCodecBlockDoubleDelta::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Worst case: header + first value + first delta + 70 bits per value
    return 1 + sizeof(UInt32) + 2 * data_bytes_size + (uncompressed_size / data_bytes_size) * 9 + 16;
}


namespace
{
    UInt8 getBlockDoubleDeltaBytesSize(const IDataType * column_type)
    {
        if (!column_type)
            return 8;
            
        if (typeid_cast<const DataTypeDateTime64 *>(column_type) ||
            typeid_cast<const DataTypeInt64 *>(column_type) ||
            typeid_cast<const DataTypeUInt64 *>(column_type))
            return 8;
        if (typeid_cast<const DataTypeInt32 *>(column_type) ||
            typeid_cast<const DataTypeUInt32 *>(column_type))
            return 4;
        if (typeid_cast<const DataTypeInt16 *>(column_type) ||
            typeid_cast<const DataTypeUInt16 *>(column_type))
            return 2;
        if (typeid_cast<const DataTypeInt8 *>(column_type) ||
            typeid_cast<const DataTypeUInt8 *>(column_type))
            return 1;
            
        return 8;
    }
}

void registerCodecBlockDoubleDelta(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::BlockDoubleDelta);
    
    auto reg_func = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        UInt8 data_bytes_size = getBlockDoubleDeltaBytesSize(column_type);
        
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() != 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE,
                    "BlockDoubleDelta codec expects 0 or 1 parameter, got {}", arguments->children.size());
            
            const auto * literal = arguments->children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "BlockDoubleDelta codec parameter must be an unsigned integer");
            
            data_bytes_size = static_cast<UInt8>(literal->value.safeGet<UInt64>());
        }
        
        if (data_bytes_size != 1 && data_bytes_size != 2 && data_bytes_size != 4 && data_bytes_size != 8)
            throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                "BlockDoubleDelta codec parameter must be 1, 2, 4, or 8, got {}", static_cast<int>(data_bytes_size));
        
        return std::make_shared<CompressionCodecBlockDoubleDelta>(data_bytes_size);
    };

    factory.registerCompressionCodecWithType("BlockDoubleDelta", method_code, reg_func);
}

}

