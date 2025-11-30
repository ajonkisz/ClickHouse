#pragma clang diagnostic ignored "-Wreserved-identifier"

#include <Common/SipHash.h>
#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <base/unaligned.h>

#include <Parsers/IAST_fwd.h>
#include <Parsers/ASTLiteral.h>

#include <IO/WriteHelpers.h>

#include <cstring>
#include <type_traits>
#include <limits>
#include <vector>
#include <unordered_map>
#include <algorithm>


namespace DB
{

/** DictionaryBlock codec - block-level dictionary encoding for columns with repeated values
 *
 * This codec is optimized for columns with low cardinality within a block,
 * such as series IDs, UUIDs, or other identifiers that repeat frequently.
 *
 * Format:
 *   [1 byte]  - flags (bit 0: is_dictionary_encoded, bits 1-7: reserved)
 *   [1 byte]  - data element size (1, 2, 4, 8, 16 bytes)
 *   [4 bytes] - item count
 *
 *   If dictionary encoded:
 *     [4 bytes] - dictionary size (number of unique values)
 *     [N * element_size] - dictionary values
 *     [variable] - indices encoded as variable-length integers
 *
 *   If not dictionary encoded (fallback):
 *     [raw data] - original data as-is
 *
 * Index encoding:
 *   Indices are encoded using minimal bytes needed:
 *   - dict_size <= 256: 1 byte per index
 *   - dict_size <= 65536: 2 bytes per index
 *   - dict_size > 65536: 4 bytes per index
 */
class CompressionCodecDictionaryBlock : public ICompressionCodec
{
public:
    explicit CompressionCodecDictionaryBlock(UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;

    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }

    String getDescription() const override
    {
        return "Block-level dictionary encoding for columns with repeated values like UUIDs or series IDs.";
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

// Flag bits
constexpr UInt8 FLAG_DICTIONARY_ENCODED = 0x01;

// Hash function for fixed-size byte arrays
struct ByteArrayHash
{
    size_t element_size;

    explicit ByteArrayHash(size_t size) : element_size(size) {}

    [[maybe_unused]] size_t operator()(const char * data) const
    {
        size_t hash = 0;
        for (size_t i = 0; i < element_size; ++i)
            hash = hash * 31 + static_cast<unsigned char>(data[i]);
        return hash;
    }
};

struct ByteArrayEqual
{
    size_t element_size;

    explicit ByteArrayEqual(size_t size) : element_size(size) {}

    [[maybe_unused]] bool operator()(const char * a, const char * b) const
    {
        return memcmp(a, b, element_size) == 0;
    }
};

template <typename ValueType>
UInt32 compressDataForType(const char * source, UInt32 source_size, char * dest)
{
    if (source_size % sizeof(ValueType) != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS,
            "Cannot compress with DictionaryBlock codec, data size {} is not aligned to {}",
            source_size, sizeof(ValueType));

    const UInt32 count = source_size / sizeof(ValueType);
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);

    if (count == 0)
    {
        // Empty block
        out[0] = 0;  // flags: not dictionary encoded
        out[1] = sizeof(ValueType);
        unalignedStoreLittleEndian<UInt32>(out + 2, 0);
        return 6;
    }

    const ValueType * in = reinterpret_cast<const ValueType *>(source);

    // Build dictionary
    std::unordered_map<ValueType, UInt32> value_to_index;
    std::vector<ValueType> dictionary;
    std::vector<UInt32> indices;
    indices.reserve(count);

    for (UInt32 i = 0; i < count; ++i)
    {
        ValueType value = unalignedLoadLittleEndian<ValueType>(in + i);
        auto it = value_to_index.find(value);
        if (it == value_to_index.end())
        {
            UInt32 idx = static_cast<UInt32>(dictionary.size());
            value_to_index[value] = idx;
            dictionary.push_back(value);
            indices.push_back(idx);
        }
        else
        {
            indices.push_back(it->second);
        }
    }

    UInt32 dict_size = static_cast<UInt32>(dictionary.size());

    // Determine index size based on dictionary size
    UInt8 index_bytes;
    if (dict_size <= 256)
        index_bytes = 1;
    else if (dict_size <= 65536)
        index_bytes = 2;
    else
        index_bytes = 4;

    // Calculate compressed size
    UInt32 dict_bytes = dict_size * sizeof(ValueType);
    UInt32 indices_bytes = count * index_bytes;
    UInt32 compressed_size = 6 + 4 + dict_bytes + indices_bytes;  // header + dict_size + dict + indices
    UInt32 uncompressed_size = source_size;

    // Only use dictionary encoding if it saves space
    // Threshold: dictionary encoding must save at least 10% space
    if (compressed_size >= uncompressed_size * 9 / 10)
    {
        // Fallback: store raw data
        out[0] = 0;  // flags: not dictionary encoded
        out[1] = sizeof(ValueType);
        unalignedStoreLittleEndian<UInt32>(out + 2, count);
        memcpy(out + 6, source, source_size);
        return 6 + source_size;
    }

    // Write dictionary-encoded data
    out[0] = FLAG_DICTIONARY_ENCODED;
    out[1] = sizeof(ValueType);
    unalignedStoreLittleEndian<UInt32>(out + 2, count);
    out += 6;

    // Write dictionary size
    unalignedStoreLittleEndian<UInt32>(out, dict_size);
    out += 4;

    // Write dictionary values
    for (const auto & value : dictionary)
    {
        unalignedStoreLittleEndian<ValueType>(out, value);
        out += sizeof(ValueType);
    }

    // Write indices
    if (index_bytes == 1)
    {
        for (UInt32 idx : indices)
            *out++ = static_cast<UInt8>(idx);
    }
    else if (index_bytes == 2)
    {
        for (UInt32 idx : indices)
        {
            unalignedStoreLittleEndian<UInt16>(out, static_cast<UInt16>(idx));
            out += 2;
        }
    }
    else
    {
        for (UInt32 idx : indices)
        {
            unalignedStoreLittleEndian<UInt32>(out, idx);
            out += 4;
        }
    }

    return static_cast<UInt32>(out - reinterpret_cast<UInt8 *>(dest));
}

// Specialization for 16-byte values (UUIDs)
UInt32 compressData16(const char * source, UInt32 source_size, char * dest)
{
    constexpr size_t value_size = 16;

    if (source_size % value_size != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS,
            "Cannot compress with DictionaryBlock codec, data size {} is not aligned to {}",
            source_size, value_size);

    const UInt32 count = source_size / value_size;
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);

    if (count == 0)
    {
        out[0] = 0;
        out[1] = value_size;
        unalignedStoreLittleEndian<UInt32>(out + 2, 0);
        return 6;
    }

    // Build dictionary using a map with custom hash/equal
    ByteArrayHash hasher(value_size);
    ByteArrayEqual equaler(value_size);

    std::unordered_map<std::string, UInt32> value_to_index;
    std::vector<std::string> dictionary;
    std::vector<UInt32> indices;
    indices.reserve(count);

    const char * in = source;
    for (UInt32 i = 0; i < count; ++i)
    {
        std::string value(in + i * value_size, value_size);
        auto it = value_to_index.find(value);
        if (it == value_to_index.end())
        {
            UInt32 idx = static_cast<UInt32>(dictionary.size());
            value_to_index[value] = idx;
            dictionary.push_back(value);
            indices.push_back(idx);
        }
        else
        {
            indices.push_back(it->second);
        }
    }

    UInt32 dict_size = static_cast<UInt32>(dictionary.size());

    // Determine index size
    UInt8 index_bytes;
    if (dict_size <= 256)
        index_bytes = 1;
    else if (dict_size <= 65536)
        index_bytes = 2;
    else
        index_bytes = 4;

    UInt32 dict_bytes = dict_size * value_size;
    UInt32 indices_bytes = count * index_bytes;
    UInt32 compressed_size = 6 + 4 + dict_bytes + indices_bytes;
    UInt32 uncompressed_size = source_size;

    // Only use dictionary if it saves space
    if (compressed_size >= uncompressed_size * 9 / 10)
    {
        out[0] = 0;
        out[1] = value_size;
        unalignedStoreLittleEndian<UInt32>(out + 2, count);
        memcpy(out + 6, source, source_size);
        return 6 + source_size;
    }

    // Write dictionary-encoded data
    out[0] = FLAG_DICTIONARY_ENCODED;
    out[1] = value_size;
    unalignedStoreLittleEndian<UInt32>(out + 2, count);
    out += 6;

    unalignedStoreLittleEndian<UInt32>(out, dict_size);
    out += 4;

    for (const auto & value : dictionary)
    {
        memcpy(out, value.data(), value_size);
        out += value_size;
    }

    if (index_bytes == 1)
    {
        for (UInt32 idx : indices)
            *out++ = static_cast<UInt8>(idx);
    }
    else if (index_bytes == 2)
    {
        for (UInt32 idx : indices)
        {
            unalignedStoreLittleEndian<UInt16>(out, static_cast<UInt16>(idx));
            out += 2;
        }
    }
    else
    {
        for (UInt32 idx : indices)
        {
            unalignedStoreLittleEndian<UInt32>(out, idx);
            out += 4;
        }
    }

    return static_cast<UInt32>(out - reinterpret_cast<UInt8 *>(dest));
}

template <typename ValueType>
void decompressDataForType(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    const UInt8 * in_end = in + source_size;

    if (source_size < 6)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: not enough data for header");

    UInt8 flags = in[0];
    UInt8 stored_size = in[1];
    UInt32 count = unalignedLoadLittleEndian<UInt32>(in + 2);
    in += 6;

    if (stored_size != sizeof(ValueType))
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: element size mismatch");

    if (count * sizeof(ValueType) != uncompressed_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: size mismatch");

    ValueType * out = reinterpret_cast<ValueType *>(dest);

    if (!(flags & FLAG_DICTIONARY_ENCODED))
    {
        // Raw data
        if (in + uncompressed_size > in_end)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
        memcpy(dest, in, uncompressed_size);
        return;
    }

    // Dictionary encoded
    if (in + 4 > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");

    UInt32 dict_size = unalignedLoadLittleEndian<UInt32>(in);
    in += 4;

    // Read dictionary
    if (in + dict_size * sizeof(ValueType) > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");

    std::vector<ValueType> dictionary(dict_size);
    for (UInt32 i = 0; i < dict_size; ++i)
    {
        dictionary[i] = unalignedLoadLittleEndian<ValueType>(in);
        in += sizeof(ValueType);
    }

    // Determine index size
    UInt8 index_bytes;
    if (dict_size <= 256)
        index_bytes = 1;
    else if (dict_size <= 65536)
        index_bytes = 2;
    else
        index_bytes = 4;

    // Read indices and reconstruct values
    for (UInt32 i = 0; i < count; ++i)
    {
        UInt32 idx;
        if (index_bytes == 1)
        {
            if (in >= in_end)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
            idx = *in++;
        }
        else if (index_bytes == 2)
        {
            if (in + 2 > in_end)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
            idx = unalignedLoadLittleEndian<UInt16>(in);
            in += 2;
        }
        else
        {
            if (in + 4 > in_end)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
            idx = unalignedLoadLittleEndian<UInt32>(in);
            in += 4;
        }

        if (idx >= dict_size)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: invalid index");

        unalignedStoreLittleEndian<ValueType>(out + i, dictionary[idx]);
    }
}

void decompressData16(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    constexpr size_t value_size = 16;

    const UInt8 * in = reinterpret_cast<const UInt8 *>(source);
    const UInt8 * in_end = in + source_size;

    if (source_size < 6)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: not enough data for header");

    UInt8 flags = in[0];
    UInt8 stored_size = in[1];
    UInt32 count = unalignedLoadLittleEndian<UInt32>(in + 2);
    in += 6;

    if (stored_size != value_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: element size mismatch");

    if (count * value_size != uncompressed_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: size mismatch");

    if (!(flags & FLAG_DICTIONARY_ENCODED))
    {
        if (in + uncompressed_size > in_end)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
        memcpy(dest, in, uncompressed_size);
        return;
    }

    if (in + 4 > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");

    UInt32 dict_size = unalignedLoadLittleEndian<UInt32>(in);
    in += 4;

    if (in + dict_size * value_size > in_end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");

    std::vector<std::string> dictionary(dict_size);
    for (UInt32 i = 0; i < dict_size; ++i)
    {
        dictionary[i] = std::string(reinterpret_cast<const char *>(in), value_size);
        in += value_size;
    }

    UInt8 index_bytes;
    if (dict_size <= 256)
        index_bytes = 1;
    else if (dict_size <= 65536)
        index_bytes = 2;
    else
        index_bytes = 4;

    char * out = dest;
    for (UInt32 i = 0; i < count; ++i)
    {
        UInt32 idx;
        if (index_bytes == 1)
        {
            if (in >= in_end)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
            idx = *in++;
        }
        else if (index_bytes == 2)
        {
            if (in + 2 > in_end)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
            idx = unalignedLoadLittleEndian<UInt16>(in);
            in += 2;
        }
        else
        {
            if (in + 4 > in_end)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: unexpected end of data");
            idx = unalignedLoadLittleEndian<UInt32>(in);
            in += 4;
        }

        if (idx >= dict_size)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock: invalid index");

        memcpy(out, dictionary[idx].data(), value_size);
        out += value_size;
    }
}

UInt8 getDataBytesSize(const IDataType * column_type)
{
    if (!column_type->isValueUnambiguouslyRepresentedInFixedSizeContiguousMemoryRegion())
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Codec DictionaryBlock is not applicable for {} because the data type is not of fixed size",
            column_type->getName());

    size_t max_size = column_type->getSizeOfValueInMemory();
    if (max_size == 1 || max_size == 2 || max_size == 4 || max_size == 8 || max_size == 16)
        return static_cast<UInt8>(max_size);

    throw Exception(
        ErrorCodes::BAD_ARGUMENTS,
        "Codec DictionaryBlock is only applicable for data types of size 1, 2, 4, 8, or 16 bytes. Given type {}",
        column_type->getName());
}

}


CompressionCodecDictionaryBlock::CompressionCodecDictionaryBlock(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    setCodecDescription("DictionaryBlock", {std::make_shared<ASTLiteral>(static_cast<UInt64>(data_bytes_size))});
}

uint8_t CompressionCodecDictionaryBlock::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::DictionaryBlock);
}

void CompressionCodecDictionaryBlock::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
    hash.update(data_bytes_size);
}

UInt32 CompressionCodecDictionaryBlock::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Worst case: header (6 bytes) + raw data
    // Plus some margin for dictionary overhead in edge cases
    return 6 + uncompressed_size + (uncompressed_size / data_bytes_size) * 4 + 4;
}

UInt32 CompressionCodecDictionaryBlock::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    UInt8 bytes_to_skip = source_size % data_bytes_size;

    // Store header for alignment handling
    dest[0] = static_cast<char>(data_bytes_size);
    dest[1] = static_cast<char>(bytes_to_skip);
    memcpy(&dest[2], source, bytes_to_skip);
    size_t start_pos = 2 + bytes_to_skip;
    UInt32 compressed_size = 0;

    switch (data_bytes_size)
    {
        case 1:
            compressed_size = compressDataForType<UInt8>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
            break;
        case 2:
            compressed_size = compressDataForType<UInt16>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
            break;
        case 4:
            compressed_size = compressDataForType<UInt32>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
            break;
        case 8:
            compressed_size = compressDataForType<UInt64>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
            break;
        case 16:
            compressed_size = compressData16(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
            break;
        default:
            throw Exception(ErrorCodes::CANNOT_COMPRESS, "Unsupported data size {} for DictionaryBlock", static_cast<int>(data_bytes_size));
    }

    return static_cast<UInt32>(2 + bytes_to_skip + compressed_size);
}

void CompressionCodecDictionaryBlock::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < 2)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock-encoded data. File has wrong header");

    UInt8 bytes_size = static_cast<UInt8>(source[0]);

    if (bytes_size == 0)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock-encoded data. File has wrong header");

    UInt8 bytes_to_skip = uncompressed_size % bytes_size;
    UInt32 output_size = uncompressed_size - bytes_to_skip;

    if (static_cast<UInt32>(2 + bytes_to_skip) > source_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock-encoded data. File has wrong header");

    memcpy(dest, &source[2], bytes_to_skip);
    UInt32 source_size_no_header = source_size - bytes_to_skip - 2;

    switch (bytes_size)
    {
        case 1:
            decompressDataForType<UInt8>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], output_size);
            break;
        case 2:
            decompressDataForType<UInt16>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], output_size);
            break;
        case 4:
            decompressDataForType<UInt32>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], output_size);
            break;
        case 8:
            decompressDataForType<UInt64>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], output_size);
            break;
        case 16:
            decompressData16(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip], output_size);
            break;
        default:
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress DictionaryBlock-encoded data. Unsupported data size {}", static_cast<int>(bytes_size));
    }
}

void registerCodecDictionaryBlock(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::DictionaryBlock);

    auto reg_func = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        /// Default bytes size is 16 (for UUIDs)
        UInt8 data_bytes_size = 16;

        if (column_type != nullptr)
            data_bytes_size = getDataBytesSize(column_type);

        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE,
                    "DictionaryBlock codec must have 1 parameter, given {}", arguments->children.size());

            const auto children = arguments->children;
            const auto * literal = children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::Which::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "DictionaryBlock codec argument must be unsigned integer");

            const size_t user_bytes_size = literal->value.safeGet<UInt64>();
            if (user_bytes_size != 1 && user_bytes_size != 2 && user_bytes_size != 4 &&
                user_bytes_size != 8 && user_bytes_size != 16)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "Argument value for DictionaryBlock codec can be 1, 2, 4, 8, or 16, given {}", user_bytes_size);

            data_bytes_size = static_cast<UInt8>(user_bytes_size);
        }

        return std::make_shared<CompressionCodecDictionaryBlock>(data_bytes_size);
    };

    factory.registerCompressionCodecWithType("DictionaryBlock", method_code, reg_func);
}

}

