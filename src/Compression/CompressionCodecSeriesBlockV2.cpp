/**
 * CompressionCodecSeriesBlockV2 - Per-series block storage
 * 
 * This codec implements VictoriaMetrics-style per-series block storage:
 * - Groups samples by series ID
 * - Stores series ID ONCE per block (not per row)
 * - Uses run-length encoding for consecutive identical IDs
 * 
 * Storage format:
 * [header]
 *   num_blocks (4 bytes)
 *   total_rows (4 bytes)
 * [for each block]
 *   series_id (16 bytes for UUID, 8 for UInt64)
 *   run_length (4 bytes) - number of consecutive rows with this ID
 * 
 * Expected compression for sorted data:
 * - UUID: 16 bytes / avg_run_length ≈ 0.01-0.05 B/row
 * - UInt64: 8 bytes / avg_run_length ≈ 0.005-0.025 B/row
 * 
 * This is effective when data is sorted by (id, timestamp) and each series
 * has multiple consecutive samples.
 */

#include <Compression/CompressionCodecEncrypted.h>
#include <Compression/CompressionFactory.h>
#include <Compression/CompressionInfo.h>
#include <base/unaligned.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTLiteral.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <Common/PODArray.h>

#include <cstring>
#include <vector>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_CODEC_PARAMETER;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
}

/**
 * Series block compression - stores ID once per run of consecutive rows
 */
class CompressionCodecSeriesBlockV2 : public ICompressionCodec
{
public:
    explicit CompressionCodecSeriesBlockV2(UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;
    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }
    String getDescription() const override { return "SeriesBlockV2 - per-series block storage with RLE"; }

private:
    UInt8 data_bytes_size;
};

CompressionCodecSeriesBlockV2::CompressionCodecSeriesBlockV2(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
    setCodecDescription("SeriesBlockV2", {std::make_shared<ASTLiteral>(static_cast<UInt64>(data_bytes_size))});
}

uint8_t CompressionCodecSeriesBlockV2::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::SeriesBlockV2);
}

void CompressionCodecSeriesBlockV2::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

UInt32 CompressionCodecSeriesBlockV2::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    // Header: 1 + 4 + 4 = 9 bytes
    // Worst case: every row is a different series
    // Each block: data_bytes_size + 4 bytes
    UInt32 max_rows = uncompressed_size / data_bytes_size;
    return 9 + max_rows * (data_bytes_size + 4);
}

UInt32 CompressionCodecSeriesBlockV2::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    if (source_size % data_bytes_size != 0)
        throw Exception(ErrorCodes::CANNOT_COMPRESS,
            "SeriesBlockV2: source size {} is not divisible by data size {}", source_size, static_cast<int>(data_bytes_size));

    const UInt32 num_rows = source_size / data_bytes_size;
    
    // Header
    dest[0] = data_bytes_size;
    unalignedStoreLittleEndian<UInt32>(dest + 1, 0);  // num_blocks (filled later)
    unalignedStoreLittleEndian<UInt32>(dest + 5, num_rows);
    
    if (num_rows == 0)
        return 9;

    // Helper to compare two values
    auto valuesEqual = [&](const char * a, const char * b) -> bool {
        return std::memcmp(a, b, data_bytes_size) == 0;
    };

    // Count blocks and write them
    UInt32 num_blocks = 0;
    UInt32 write_pos = 9;
    UInt32 i = 0;
    
    while (i < num_rows)
    {
        const char * current_id = source + i * data_bytes_size;
        UInt32 run_length = 1;
        
        // Count consecutive identical IDs
        while (i + run_length < num_rows && 
               valuesEqual(current_id, source + (i + run_length) * data_bytes_size))
        {
            ++run_length;
        }
        
        // Write block: ID + run_length
        std::memcpy(dest + write_pos, current_id, data_bytes_size);
        write_pos += data_bytes_size;
        unalignedStoreLittleEndian<UInt32>(dest + write_pos, run_length);
        write_pos += 4;
        
        ++num_blocks;
        i += run_length;
    }
    
    // Update num_blocks in header
    unalignedStoreLittleEndian<UInt32>(dest + 1, num_blocks);
    
    return write_pos;
}

void CompressionCodecSeriesBlockV2::doDecompressData(
    const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < 9)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlockV2: source too small");

    UInt8 bytes_size = source[0];
    UInt32 num_blocks = unalignedLoadLittleEndian<UInt32>(source + 1);
    UInt32 total_rows = unalignedLoadLittleEndian<UInt32>(source + 5);
    
    if (total_rows == 0)
        return;

    if (uncompressed_size != total_rows * bytes_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS,
            "SeriesBlockV2: size mismatch {} != {} * {}", uncompressed_size, total_rows, static_cast<int>(bytes_size));

    UInt32 read_pos = 9;
    UInt32 write_pos = 0;
    
    for (UInt32 block = 0; block < num_blocks; ++block)
    {
        if (read_pos + bytes_size + 4 > source_size)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "SeriesBlockV2: unexpected end of data");
        
        const char * series_id = source + read_pos;
        read_pos += bytes_size;
        
        UInt32 run_length = unalignedLoadLittleEndian<UInt32>(source + read_pos);
        read_pos += 4;
        
        // Expand run
        for (UInt32 j = 0; j < run_length; ++j)
        {
            std::memcpy(dest + write_pos, series_id, bytes_size);
            write_pos += bytes_size;
        }
    }
    
    if (write_pos != uncompressed_size)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS,
            "SeriesBlockV2: decompressed size mismatch {} != {}", write_pos, uncompressed_size);
}

void registerCodecSeriesBlockV2(CompressionCodecFactory & factory)
{
    auto method_byte = static_cast<UInt8>(CompressionMethodByte::SeriesBlockV2);
    
    auto creator = [&](const ASTPtr & arguments) -> CompressionCodecPtr
    {
        UInt8 data_bytes_size = 16; // Default to 16 bytes (UUID)
        
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE,
                    "SeriesBlockV2 codec accepts at most 1 argument");
            
            const auto * literal = arguments->children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "SeriesBlockV2 codec argument must be unsigned integer");
            
            data_bytes_size = static_cast<UInt8>(literal->value.safeGet<UInt64>());
        }
        
        return std::make_shared<CompressionCodecSeriesBlockV2>(data_bytes_size);
    };

    factory.registerCompressionCodecWithType("SeriesBlockV2", method_byte,
        [&](const ASTPtr & arguments, const IDataType *) -> CompressionCodecPtr
        {
            return creator(arguments);
        });
}

} // namespace DB

