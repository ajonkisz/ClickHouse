#pragma clang diagnostic ignored "-Wreserved-identifier"

#include <Common/SipHash.h>
#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <base/unaligned.h>
#include <Parsers/IAST_fwd.h>
#include <Parsers/ASTLiteral.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/BitHelpers.h>

#include <bitset>
#include <cstring>
#include <algorithm>
#include <type_traits>


namespace DB
{

/** GorillaV2 - Enhanced Gorilla codec with optimized leading/trailing zero handling
 *
 * Improvements over standard Gorilla:
 * 1. Uses a more compact encoding when XOR value fits within previous meaningful bits window
 * 2. Better bit packing for common patterns
 * 3. Optimized for slowly changing floating-point values
 *
 * Encoding scheme:
 *   0             - XOR is zero (repeated value), 1 bit
 *   10 + bits     - XOR fits in previous window, 2 + meaningful_bits
 *   11 + 5 + 6 + bits - New window: 2 + 5 (leading) + 6 (length) + meaningful_bits
 */
class CompressionCodecGorillaV2 : public ICompressionCodec
{
public:
    explicit CompressionCodecGorillaV2(UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;

    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }
    bool isFloatingPointTimeSeriesCodec() const override { return true; }

    String getDescription() const override
    {
        return "Enhanced Gorilla XOR compression with optimized bit packing; ideal for slowly changing numbers.";
    }

private:
    const UInt8 data_bytes_size;
};


namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int BAD_ARGUMENTS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
}

namespace
{

constexpr UInt8 getBitLengthOfLength(UInt8 data_bytes_size)
{
    // 1-byte value is 8 bits, and we need 4 bits to represent 8 : 1000,
    // 2-byte         16 bits        =>    5
    // 4-byte         32 bits        =>    6
    // 8-byte         64 bits        =>    7
    const UInt8 bit_lengths[] = {0, 4, 5, 0, 6, 0, 0, 0, 7};
    assert(data_bytes_size >= 1 && data_bytes_size < sizeof(bit_lengths) && bit_lengths[data_bytes_size] != 0);
    return bit_lengths[data_bytes_size];
}

UInt32 getCompressedHeaderSize(UInt8 data_bytes_size)
{
    constexpr UInt8 items_count_size = 4;
    return items_count_size + data_bytes_size;
}

UInt32 getCompressedDataSize(UInt8 data_bytes_size, UInt32 uncompressed_size)
{
    const UInt32 items_count = uncompressed_size / data_bytes_size;

    static const auto DATA_BIT_LENGTH = getBitLengthOfLength(data_bytes_size);
    // -1 since there must be at least 1 non-zero bit.
    static const auto LEADING_ZEROES_BIT_LENGTH = DATA_BIT_LENGTH - 1;

    // worst case (for 32-bit value):
    // 11 + 5 bits of leading zeroes bit-size + 6 bits of data bit-size + non-zero data bits.
    const UInt32 max_item_size_bits = 2 + LEADING_ZEROES_BIT_LENGTH + DATA_BIT_LENGTH + data_bytes_size * 8;

    // + 8 is to round up to next byte.
    return (items_count * max_item_size_bits + 8) / 8;
}

struct BinaryValueInfo
{
    UInt8 leading_zero_bits;
    UInt8 data_bits;
    UInt8 trailing_zero_bits;
};

template <typename T>
BinaryValueInfo getBinaryValueInfo(const T & value)
{
    constexpr UInt8 bit_size = sizeof(T) * 8;

    const UInt8 lz = getLeadingZeroBits(value);
    const UInt8 tz = getTrailingZeroBits(value);
    const UInt8 data_size = value == 0 ? 0 : static_cast<UInt8>(bit_size - lz - tz);

    return {lz, data_size, tz};
}

template <typename T>
UInt32 compressDataForType(const char * source, UInt32 source_size, char * dest, UInt32 dest_size)
{
    if (source_size % sizeof(T) != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with GorillaV2 codec, data size {} is not aligned to {}", source_size, sizeof(T));

    const char * const source_end = source + source_size;
    const char * const dest_start = dest;
    const char * const dest_end = dest + dest_size;

    const UInt32 items_count = source_size / sizeof(T);

    unalignedStoreLittleEndian<UInt32>(dest, items_count);
    dest += sizeof(items_count);

    T prev_value = 0;
    // That would cause first XORed value to be written in-full.
    BinaryValueInfo prev_xored_info{0, 0, 0};

    if (source < source_end)
    {
        prev_value = unalignedLoadLittleEndian<T>(source);
        unalignedStoreLittleEndian<T>(dest, prev_value);

        source += sizeof(prev_value);
        dest += sizeof(prev_value);
    }

    BitWriter writer(dest, dest_end - dest);

    static const auto DATA_BIT_LENGTH = getBitLengthOfLength(sizeof(T));
    // -1 since there must be at least 1 non-zero bit.
    static const auto LEADING_ZEROES_BIT_LENGTH = DATA_BIT_LENGTH - 1;

    while (source < source_end)
    {
        const T curr_value = unalignedLoadLittleEndian<T>(source);
        source += sizeof(curr_value);

        const auto xored_data = curr_value ^ prev_value;
        const BinaryValueInfo curr_xored_info = getBinaryValueInfo(xored_data);

        if (xored_data == 0)
        {
            // Repeated value: just 1 bit
            writer.writeBits(1, 0);
        }
        else if (prev_xored_info.data_bits != 0
                && prev_xored_info.leading_zero_bits <= curr_xored_info.leading_zero_bits
                && prev_xored_info.trailing_zero_bits <= curr_xored_info.trailing_zero_bits)
        {
            // XOR fits in previous window: 2-bit prefix + meaningful bits
            writer.writeBits(2, 0b10);
            writer.writeBits(prev_xored_info.data_bits, xored_data >> prev_xored_info.trailing_zero_bits);
        }
        else
        {
            // New window: 2-bit prefix + leading zeros + length + meaningful bits
            writer.writeBits(2, 0b11);
            writer.writeBits(LEADING_ZEROES_BIT_LENGTH, curr_xored_info.leading_zero_bits);
            writer.writeBits(DATA_BIT_LENGTH, curr_xored_info.data_bits);
            writer.writeBits(curr_xored_info.data_bits, xored_data >> curr_xored_info.trailing_zero_bits);
            prev_xored_info = curr_xored_info;
        }

        prev_value = curr_value;
    }

    writer.flush();

    return static_cast<UInt32>((dest - dest_start) + (writer.count() + 7) / 8);
}

template <typename T>
void decompressDataForType(const char * source, UInt32 source_size, char * dest, UInt32 dest_size)
{
    const char * const source_end = source + source_size;

    if (source + sizeof(UInt32) > source_end)
        return;

    const UInt32 items_count = unalignedLoadLittleEndian<UInt32>(source);
    source += sizeof(items_count);

    T prev_value = 0;

    // decoding first item
    if (source + sizeof(T) > source_end || items_count < 1)
        return;

    if (static_cast<UInt64>(items_count) * sizeof(T) > dest_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data: corrupted input data.");

    prev_value = unalignedLoadLittleEndian<T>(source);
    unalignedStoreLittleEndian<T>(dest, prev_value);

    source += sizeof(prev_value);
    dest += sizeof(prev_value);

    BitReader reader(source, source_size - sizeof(items_count) - sizeof(prev_value));

    BinaryValueInfo prev_xored_info{0, 0, 0};

    static const auto DATA_BIT_LENGTH = getBitLengthOfLength(sizeof(T));
    // -1 since there must be at least 1 non-zero bit.
    static const auto LEADING_ZEROES_BIT_LENGTH = DATA_BIT_LENGTH - 1;

    // since data is tightly packed, up to 1 bit per value, and last byte is padded with zeroes,
    // we have to keep track of items to avoid reading more that there is.
    for (UInt32 items_read = 1; items_read < items_count && !reader.eof(); ++items_read)
    {
        T curr_value = prev_value;
        BinaryValueInfo curr_xored_info = prev_xored_info;
        T xored_data = 0;

        if (reader.readBit() == 1)
        {
            if (reader.readBit() == 1)
            {
                // 0b11 prefix - new window
                curr_xored_info.leading_zero_bits = reader.readBits(LEADING_ZEROES_BIT_LENGTH);
                curr_xored_info.data_bits = reader.readBits(DATA_BIT_LENGTH);
                curr_xored_info.trailing_zero_bits = sizeof(T) * 8 - curr_xored_info.leading_zero_bits - curr_xored_info.data_bits;
            }
            // else: 0b10 prefix - use prev_xored_info

            if (curr_xored_info.leading_zero_bits == 0
                && curr_xored_info.data_bits == 0
                && curr_xored_info.trailing_zero_bits == 0) [[unlikely]]
            {
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data: corrupted input data.");
            }

            xored_data = static_cast<T>(reader.readBits(curr_xored_info.data_bits));
            xored_data <<= curr_xored_info.trailing_zero_bits;
            curr_value = prev_value ^ xored_data;
        }
        // else: 0b0 prefix - use prev_value (repeated)

        unalignedStoreLittleEndian<T>(dest, curr_value);
        dest += sizeof(curr_value);

        prev_xored_info = curr_xored_info;
        prev_value = curr_value;
    }
}

UInt8 getDataBytesSize(const IDataType * column_type)
{
    if (!column_type->isValueUnambiguouslyRepresentedInFixedSizeContiguousMemoryRegion())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Codec GorillaV2 is not applicable for {} because the data type is not of fixed size",
            column_type->getName());

    size_t max_size = column_type->getSizeOfValueInMemory();
    if (max_size == 1 || max_size == 2 || max_size == 4 || max_size == 8)
        return static_cast<UInt8>(max_size);
    throw Exception(
        ErrorCodes::BAD_ARGUMENTS,
        "Codec GorillaV2 is only applicable for data types of size 1, 2, 4, 8 bytes. Given type {}",
        column_type->getName());
}

}


CompressionCodecGorillaV2::CompressionCodecGorillaV2(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    setCodecDescription("GorillaV2", {std::make_shared<ASTLiteral>(static_cast<UInt64>(data_bytes_size))});
}

uint8_t CompressionCodecGorillaV2::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::GorillaV2);
}

void CompressionCodecGorillaV2::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
    hash.update(data_bytes_size);
}

UInt32 CompressionCodecGorillaV2::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    const auto result = 2 // common header
            + data_bytes_size // max bytes skipped if source is not properly aligned.
            + getCompressedHeaderSize(data_bytes_size) // data-specific header
            + getCompressedDataSize(data_bytes_size, uncompressed_size);

    return result;
}

UInt32 CompressionCodecGorillaV2::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    UInt8 bytes_to_skip = source_size % data_bytes_size;
    dest[0] = static_cast<char>(data_bytes_size);
    dest[1] = static_cast<char>(bytes_to_skip); /// unused (backward compatibility)
    memcpy(&dest[2], source, bytes_to_skip);
    size_t start_pos = 2 + bytes_to_skip;
    UInt32 result_size = 0;

    const UInt32 compressed_size = getMaxCompressedDataSize(source_size);
    switch (data_bytes_size)
    {
        case 1:
            result_size = compressDataForType<UInt8>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos], compressed_size);
            break;
        case 2:
            result_size = compressDataForType<UInt16>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos], compressed_size);
            break;
        case 4:
            result_size = compressDataForType<UInt32>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos], compressed_size);
            break;
        case 8:
            result_size = compressDataForType<UInt64>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos], compressed_size);
            break;
        default:
            throw Exception(ErrorCodes::CANNOT_COMPRESS, "Unsupported data size {} for GorillaV2", static_cast<int>(data_bytes_size));
    }

    return 2 + bytes_to_skip + result_size;
}

void CompressionCodecGorillaV2::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < 2)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data. File has wrong header");

    UInt8 bytes_size = static_cast<UInt8>(source[0]);

    if (bytes_size == 0)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data. File has wrong header");

    UInt8 bytes_to_skip = uncompressed_size % bytes_size;

    if (static_cast<UInt32>(2 + bytes_to_skip) > source_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data. File has wrong header");

    if (bytes_to_skip >= uncompressed_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data. File has wrong header");

    memcpy(dest, &source[2], bytes_to_skip);
    UInt32 source_size_no_header = source_size - bytes_to_skip - 2;
    UInt32 uncompressed_size_left = uncompressed_size - bytes_to_skip;

    switch (bytes_size)
    {
        case 1:
            decompressDataForType<UInt8>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], uncompressed_size_left);
            break;
        case 2:
            decompressDataForType<UInt16>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], uncompressed_size_left);
            break;
        case 4:
            decompressDataForType<UInt32>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], uncompressed_size_left);
            break;
        case 8:
            decompressDataForType<UInt64>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], uncompressed_size_left);
            break;
        default:
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress GorillaV2-encoded data. Unsupported data size {}", static_cast<int>(bytes_size));
    }
}

void registerCodecGorillaV2(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::GorillaV2);

    auto codec_builder = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        /// Default bytes size is 8 (for Float64)
        UInt8 data_bytes_size = 8;

        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "GorillaV2 codec must have 1 parameter, given {}", arguments->children.size());

            const auto children = arguments->children;
            const auto * literal = children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::Which::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "GorillaV2 codec argument must be unsigned integer");

            size_t user_bytes_size = literal->value.safeGet<UInt64>();
            if (user_bytes_size != 1 && user_bytes_size != 2 && user_bytes_size != 4 && user_bytes_size != 8)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "Argument value for GorillaV2 codec can be 1, 2, 4 or 8, given {}", user_bytes_size);
            data_bytes_size = static_cast<UInt8>(user_bytes_size);
        }
        else if (column_type)
        {
            data_bytes_size = getDataBytesSize(column_type);
        }

        return std::make_shared<CompressionCodecGorillaV2>(data_bytes_size);
    };

    factory.registerCompressionCodecWithType("GorillaV2", method_code, codec_builder);
}

}

