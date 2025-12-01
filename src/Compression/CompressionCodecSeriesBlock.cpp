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
#include <DataTypes/DataTypeUUID.h>

#include <cstring>
#include <type_traits>
#include <limits>
#include <vector>
#include <unordered_map>
#include <algorithm>


namespace DB
{

/** SeriesBlock codec - Optimized for time-series ID columns
 *
 * This codec is designed for columns where the same values repeat many times
 * in sorted order (like series IDs in time-series data ordered by (id, timestamp)).
 *
 * Block format:
 *   [1 byte: mode]
 *   Mode 0 (single value): [16 bytes: value] - all rows have same value
 *   Mode 1 (RLE): [4 bytes: count] + RLE encoded (value, run_length) pairs
 *   Mode 2 (dictionary): [4 bytes: dict_size] + dictionary + varint indices
 *
 * For time-series data sorted by (id, timestamp), the same ID repeats for
 * hundreds or thousands of consecutive rows, making RLE very effective.
 */
class CompressionCodecSeriesBlock : public ICompressionCodec
{
public:
    explicit CompressionCodecSeriesBlock(UInt8 data_bytes_size_);

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
        return "Run-length and dictionary encoding optimized for time-series ID columns.";
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
// Varint encoding for run lengths
//------------------------------------------------------------------------------

inline size_t encodeVarUInt(UInt64 value, UInt8 * dest)
{
    size_t pos = 0;
    while (value >= 0x80)
    {
        dest[pos++] = static_cast<UInt8>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    dest[pos++] = static_cast<UInt8>(value);
    return pos;
}

inline size_t decodeVarUInt(const UInt8 * src, const UInt8 * src_end, UInt64 & value)
{
    value = 0;
    size_t pos = 0;
    size_t shift = 0;
    
    while (pos < 10 && src + pos < src_end)
    {
        UInt8 byte = src[pos];
        value |= static_cast<UInt64>(byte & 0x7F) << shift;
        pos++;
        if ((byte & 0x80) == 0)
            return pos;
        shift += 7;
    }
    
    throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock: invalid varint");
}

//------------------------------------------------------------------------------
// Mode 0: Single value (all same)
//------------------------------------------------------------------------------

template <typename T>
bool trySingleValueCompress(const T * data, UInt32 count, UInt8 * dest, UInt32 & output_size)
{
    if (count == 0)
    {
        output_size = 0;
        return true;
    }
    
    T first_value = unalignedLoadLittleEndian<T>(data);
    
    for (UInt32 i = 1; i < count; ++i)
    {
        if (unalignedLoadLittleEndian<T>(data + i) != first_value)
            return false;
    }
    
    // All values are the same
    dest[0] = 0;  // Mode 0
    unalignedStoreLittleEndian<UInt32>(dest + 1, count);
    unalignedStoreLittleEndian<T>(dest + 5, first_value);
    output_size = 5 + sizeof(T);
    return true;
}

//------------------------------------------------------------------------------
// Mode 1: Run-Length Encoding
//------------------------------------------------------------------------------

template <typename T>
UInt32 rleCompress(const T * data, UInt32 count, UInt8 * dest)
{
    dest[0] = 1;  // Mode 1
    unalignedStoreLittleEndian<UInt32>(dest + 1, count);
    UInt8 * out = dest + 5;
    
    UInt32 i = 0;
    while (i < count)
    {
        T value = unalignedLoadLittleEndian<T>(data + i);
        UInt32 run_length = 1;
        
        while (i + run_length < count && 
               unalignedLoadLittleEndian<T>(data + i + run_length) == value)
        {
            run_length++;
        }
        
        // Write value
        unalignedStoreLittleEndian<T>(out, value);
        out += sizeof(T);
        
        // Write run length as varint
        out += encodeVarUInt(run_length, out);
        
        i += run_length;
    }
    
    return static_cast<UInt32>(out - dest);
}

template <typename T>
void rleDecompress(const UInt8 * src, UInt32 src_size, T * dest, UInt32 expected_count)
{
    if (src_size < 5)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock RLE: insufficient data");
    
    UInt32 count = unalignedLoadLittleEndian<UInt32>(src + 1);
    if (count != expected_count)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock RLE: count mismatch");
    
    const UInt8 * in = src + 5;
    const UInt8 * in_end = src + src_size;
    UInt32 written = 0;
    
    while (written < count && in < in_end)
    {
        if (in + sizeof(T) > in_end)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock RLE: unexpected end of data");
        
        T value = unalignedLoadLittleEndian<T>(reinterpret_cast<const T *>(in));
        in += sizeof(T);
        
        UInt64 run_length;
        in += decodeVarUInt(in, in_end, run_length);
        
        if (written + run_length > count)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock RLE: run length exceeds count");
        
        for (UInt64 j = 0; j < run_length; ++j)
        {
            unalignedStoreLittleEndian<T>(dest + written, value);
            written++;
        }
    }
    
    if (written != count)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock RLE: incomplete decompression");
}

//------------------------------------------------------------------------------
// Mode 2: Dictionary encoding
//------------------------------------------------------------------------------

template <typename T>
UInt32 dictCompress(const T * data, UInt32 count, UInt8 * dest)
{
    // Build dictionary
    std::vector<T> dict;
    std::unordered_map<T, UInt32> value_to_idx;
    
    for (UInt32 i = 0; i < count; ++i)
    {
        T value = unalignedLoadLittleEndian<T>(data + i);
        if (value_to_idx.find(value) == value_to_idx.end())
        {
            value_to_idx[value] = static_cast<UInt32>(dict.size());
            dict.push_back(value);
        }
    }
    
    dest[0] = 2;  // Mode 2
    unalignedStoreLittleEndian<UInt32>(dest + 1, count);
    unalignedStoreLittleEndian<UInt32>(dest + 5, static_cast<UInt32>(dict.size()));
    UInt8 * out = dest + 9;
    
    // Write dictionary
    for (const T & value : dict)
    {
        unalignedStoreLittleEndian<T>(out, value);
        out += sizeof(T);
    }
    
    // Write indices as varints
    for (UInt32 i = 0; i < count; ++i)
    {
        T value = unalignedLoadLittleEndian<T>(data + i);
        UInt32 idx = value_to_idx[value];
        out += encodeVarUInt(idx, out);
    }
    
    return static_cast<UInt32>(out - dest);
}

template <typename T>
void dictDecompress(const UInt8 * src, UInt32 src_size, T * dest, UInt32 expected_count)
{
    if (src_size < 9)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock dict: insufficient data");
    
    UInt32 count = unalignedLoadLittleEndian<UInt32>(src + 1);
    UInt32 dict_size = unalignedLoadLittleEndian<UInt32>(src + 5);
    
    if (count != expected_count)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock dict: count mismatch");
    
    const UInt8 * in = src + 9;
    const UInt8 * in_end = src + src_size;
    
    // Read dictionary
    std::vector<T> dict(dict_size);
    for (UInt32 i = 0; i < dict_size; ++i)
    {
        if (in + sizeof(T) > in_end)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock dict: unexpected end in dictionary");
        dict[i] = unalignedLoadLittleEndian<T>(reinterpret_cast<const T *>(in));
        in += sizeof(T);
    }
    
    // Read indices and decompress
    for (UInt32 i = 0; i < count; ++i)
    {
        UInt64 idx;
        in += decodeVarUInt(in, in_end, idx);
        
        if (idx >= dict_size)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock dict: index out of bounds");
        
        unalignedStoreLittleEndian<T>(dest + i, dict[idx]);
    }
}

//------------------------------------------------------------------------------
// Choose best compression method
//------------------------------------------------------------------------------

template <typename T>
UInt32 compressDataForType(const char * source, UInt32 source_size, char * dest)
{
    if (source_size % sizeof(T) != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "SeriesBlock: data size not aligned");
    
    const UInt32 count = source_size / sizeof(T);
    const T * data = reinterpret_cast<const T *>(source);
    UInt8 * out = reinterpret_cast<UInt8 *>(dest);
    
    if (count == 0)
    {
        out[0] = 0;
        unalignedStoreLittleEndian<UInt32>(out + 1, 0);
        return 5;
    }
    
    // Try single value compression
    UInt32 single_size;
    if (trySingleValueCompress(data, count, out, single_size))
        return single_size;
    
    // Count unique values and runs
    UInt32 unique_count = 0;
    UInt32 run_count = 0;
    {
        std::unordered_map<T, bool> seen;
        T prev = unalignedLoadLittleEndian<T>(data);
        seen[prev] = true;
        unique_count = 1;
        run_count = 1;
        
        for (UInt32 i = 1; i < count; ++i)
        {
            T curr = unalignedLoadLittleEndian<T>(data + i);
            if (curr != prev)
            {
                run_count++;
                if (seen.find(curr) == seen.end())
                {
                    seen[curr] = true;
                    unique_count++;
                }
                prev = curr;
            }
        }
    }
    
    // Estimate sizes
    // RLE: mode + count + (value + varint_run_length) per run
    UInt32 rle_est = 5 + run_count * (sizeof(T) + 2);
    
    // Dict: mode + count + dict_size + dict + varint indices
    UInt32 dict_idx_bits = unique_count <= 1 ? 1 : (32 - __builtin_clz(unique_count - 1));
    UInt32 dict_est = 9 + unique_count * sizeof(T) + (count * (dict_idx_bits + 7)) / 8;
    
    // Choose better method
    if (rle_est <= dict_est)
    {
        return rleCompress(data, count, out);
    }
    else
    {
        return dictCompress(data, count, out);
    }
}

template <typename T>
void decompressDataForType(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    if (source_size < 1)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock: insufficient data");
    
    const UInt8 * src = reinterpret_cast<const UInt8 *>(source);
    T * out = reinterpret_cast<T *>(dest);
    UInt32 expected_count = uncompressed_size / sizeof(T);
    
    UInt8 mode = src[0];
    
    switch (mode)
    {
        case 0:  // Single value
        {
            if (source_size < 5 + sizeof(T))
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock mode 0: insufficient data");
            
            UInt32 count = unalignedLoadLittleEndian<UInt32>(src + 1);
            if (count != expected_count)
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock mode 0: count mismatch");
            
            T value = unalignedLoadLittleEndian<T>(reinterpret_cast<const T *>(src + 5));
            for (UInt32 i = 0; i < count; ++i)
                unalignedStoreLittleEndian<T>(out + i, value);
            break;
        }
        case 1:  // RLE
            rleDecompress(src, source_size, out, expected_count);
            break;
        case 2:  // Dictionary
            dictDecompress(src, source_size, out, expected_count);
            break;
        default:
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlock: unknown mode {}", static_cast<int>(mode));
    }
}

} // anonymous namespace


CompressionCodecSeriesBlock::CompressionCodecSeriesBlock(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    setCodecDescription("SeriesBlock", {std::make_shared<ASTLiteral>(static_cast<UInt64>(data_bytes_size))});
}

uint8_t CompressionCodecSeriesBlock::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::SeriesBlock);
}

void CompressionCodecSeriesBlock::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

UInt32 CompressionCodecSeriesBlock::doCompressData(const char * source, UInt32 source_size, char * dest) const
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
            case 16:
                // For UUID (128-bit), treat as two UInt64s or use special handling
                // Fall through to use simple copy for now - UUID should use ZSTD
                memcpy(dest + output_size, source, source_size_aligned);
                output_size += source_size_aligned;
                break;
            default:
                throw Exception(ErrorCodes::CANNOT_COMPRESS, "Unsupported data size {} for SeriesBlock", static_cast<int>(data_bytes_size));
        }

        if (bytes_to_skip != 0)
        {
            memcpy(dest + output_size, source + source_size_aligned, bytes_to_skip);
            output_size += bytes_to_skip;
        }
    }

    return output_size;
}

void CompressionCodecSeriesBlock::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < 1)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress SeriesBlock: source buffer too small");

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
            case 16:
                memcpy(dest, source + source_offset, uncompressed_size_aligned);
                break;
            default:
                throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Unsupported data size {} for SeriesBlock", static_cast<int>(data_bytes_size));
        }

        if (bytes_to_skip != 0)
        {
            memcpy(dest + uncompressed_size_aligned, source + source_size - bytes_to_skip, bytes_to_skip);
        }
    }
}

UInt32 CompressionCodecSeriesBlock::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Worst case: mode + count + uncompressed data + overhead
    return 1 + sizeof(UInt32) + uncompressed_size + 64;
}


void registerCodecSeriesBlock(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::SeriesBlock);
    
    auto reg_func = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        UInt8 data_bytes_size = 8; // Default to 8 bytes (UInt64)
        
        if (column_type)
        {
            if (typeid_cast<const DataTypeUUID *>(column_type))
                data_bytes_size = 16;
            else if (typeid_cast<const DataTypeUInt64 *>(column_type) || typeid_cast<const DataTypeInt64 *>(column_type))
                data_bytes_size = 8;
            else if (typeid_cast<const DataTypeUInt32 *>(column_type) || typeid_cast<const DataTypeInt32 *>(column_type))
                data_bytes_size = 4;
            else if (typeid_cast<const DataTypeUInt16 *>(column_type) || typeid_cast<const DataTypeInt16 *>(column_type))
                data_bytes_size = 2;
            else if (typeid_cast<const DataTypeUInt8 *>(column_type) || typeid_cast<const DataTypeInt8 *>(column_type))
                data_bytes_size = 1;
        }
        
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() != 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE,
                    "SeriesBlock codec expects 0 or 1 parameter, got {}", arguments->children.size());
            
            const auto * literal = arguments->children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "SeriesBlock codec parameter must be an unsigned integer");
            
            data_bytes_size = static_cast<UInt8>(literal->value.safeGet<UInt64>());
        }
        
        if (data_bytes_size != 1 && data_bytes_size != 2 && data_bytes_size != 4 && 
            data_bytes_size != 8 && data_bytes_size != 16)
            throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                "SeriesBlock codec parameter must be 1, 2, 4, 8, or 16, got {}", static_cast<int>(data_bytes_size));
        
        return std::make_shared<CompressionCodecSeriesBlock>(data_bytes_size);
    };

    factory.registerCompressionCodecWithType("SeriesBlock", method_code, reg_func);
}

}

