# ClickHouse TimeSeries - Improvements to Match VictoriaMetrics

**Date:** December 1, 2025  
**Current Performance:** 1.57 bytes/sample  
**Target Performance:** ~0.45 bytes/sample (competitive with VM's 0.4 B)

---

## Current Status

### What's Working ✅

| Feature | Status |
|---------|--------|
| Remote Write Ingestion | ✅ Working (30M+ rows) |
| PromQL Instant Query | ✅ Working |
| PromQL Range Query | ✅ Working |
| Labels Endpoint | ✅ Working |
| Label Values Endpoint | ✅ Working |
| Series Endpoint | ✅ Working |
| SQL SELECT on TimeSeries | ✅ Working |
| DoubleDeltaVarInt codec | ✅ Working (0.62 B/row) |
| GorillaV2 codec | ✅ Working (0.51 B/row) |
| Grafana Integration | ✅ Working |

### Current Compression Stats

| Column | Codec | Bytes/Row | Target | Gap |
|--------|-------|-----------|--------|-----|
| timestamp | DoubleDeltaVarInt | 0.62 | 0.15 | 4x |
| value | GorillaV2 | 0.51 | 0.25 | 2x |
| id | ZSTD | 0.17 | 0.05 | 3x |
| **TOTAL** | - | **1.57** | **0.45** | **3.5x** |

---

## Priority 1: Block-Level Timestamp Compression

### Problem

DoubleDeltaVarInt encodes each timestamp independently with byte-aligned output:
- Minimum 1 byte per timestamp even for delta-of-delta = 0
- VictoriaMetrics uses 1 BIT for delta-of-delta = 0

### Solution: Implement BlockDoubleDelta Codec

**File:** `src/Compression/CompressionCodecBlockDoubleDelta.cpp`

**Encoding scheme:**
```cpp
// Block structure:
// [4 bytes: block size]
// [8 bytes: first timestamp]
// [8 bytes: second timestamp]
// [variable: bit-packed delta-of-deltas]

// Per-value encoding (bit-level):
// delta-of-delta = 0:           1 bit  (0)
// |dod| in [1, 63]:             8 bits (10 + 6-bit value)
// |dod| in [64, 255]:           12 bits (110 + 8-bit value)
// |dod| in [256, 2047]:         16 bits (1110 + 11-bit value)
// |dod| in [2048, 65535]:       20 bits (11110 + 15-bit value)
// otherwise:                    36 bits (11111 + 32-bit value)
```

**Expected improvement:** 0.62 → 0.15 bytes/row (4x better)

**Implementation steps:**
1. Create new codec class `CompressionCodecBlockDoubleDelta`
2. Register codec in `registerCodecBlockDoubleDelta()`
3. Add to `CompressionCodecFactory`
4. Implement `doCompressData()` with bit-packing
5. Implement `doDecompressData()` with bit-unpacking
6. Add tests in `src/Compression/tests/`

**Test:**
```sql
CREATE TABLE test.block_dd (
    ts DateTime64(3) CODEC(BlockDoubleDelta)
) ENGINE = MergeTree ORDER BY ts;

INSERT INTO test.block_dd 
SELECT toDateTime64(now(), 3) + number * 15 
FROM numbers(1000000);

SELECT 
    column,
    sum(column_data_compressed_bytes) / sum(rows) as bytes_per_row
FROM system.parts_columns
WHERE database = 'test' AND table = 'block_dd' AND active
GROUP BY column;
```

---

## Priority 2: Block-Level Value Compression

### Problem

GorillaV2 encodes each XOR independently with byte-aligned output:
- Minimum overhead per value even for XOR = 0
- VictoriaMetrics uses 1 BIT for XOR = 0

### Solution: Implement BlockGorilla Codec

**File:** `src/Compression/CompressionCodecBlockGorilla.cpp`

**Encoding scheme:**
```cpp
// Block structure:
// [4 bytes: block size]
// [8 bytes: first value]
// [variable: bit-packed XOR values]

// Per-value encoding (bit-level):
// XOR = 0 (same value):         1 bit  (0)
// Same leading/trailing zeros:  2 bits (10) + meaningful bits
// New zero pattern:             2+5+6 bits (11 + leading + trailing) + meaningful bits
```

**Expected improvement:** 0.51 → 0.25 bytes/row (2x better)

**Implementation steps:**
1. Create new codec class `CompressionCodecBlockGorilla`
2. Track leading zeros and trailing zeros across values
3. Use bit-level I/O for encoding/decoding
4. Maintain block boundaries for random access

---

## Priority 3: Per-Block Series ID

### Problem

Currently storing UUID (16 bytes) per row, even with ZSTD:
- ZSTD achieves 92x compression → 0.17 bytes/row
- VictoriaMetrics stores series ID once per block (~0.0001 B/row)

### Solution: Implement SeriesBlockCodec

**File:** `src/Compression/CompressionCodecSeriesBlock.cpp`

**Encoding scheme:**
```cpp
// Option A: Single series per block
// [16 bytes: series UUID]
// [all rows use this UUID]

// Option B: Dictionary per block
// [2 bytes: dictionary size]
// [N * 16 bytes: unique UUIDs]
// [rows.size() * ceil(log2(N)) bits: indices]
```

**Expected improvement:** 0.17 → 0.05 bytes/row (3x better)

**Challenge:** Requires coordination with MergeTree to ensure rows are sorted by series ID within blocks.

---

## Priority 4: Bit-Level I/O Utilities

### Problem

Current codecs use byte-aligned I/O, wasting bits.

### Solution: Create BitWriter/BitReader classes

**File:** `src/IO/BitIO.h`

```cpp
class BitWriter {
    std::vector<uint8_t> buffer;
    size_t bit_position = 0;
    
public:
    void writeBits(uint64_t value, size_t num_bits);
    void writeVarInt(int64_t value);  // Sign-magnitude or zigzag
    std::vector<uint8_t> finish();
};

class BitReader {
    const uint8_t* data;
    size_t bit_position = 0;
    
public:
    uint64_t readBits(size_t num_bits);
    int64_t readVarInt();
};
```

---

## Priority 5: Tags Table Optimization

### Problem

Tags table uses 43 bytes per unique series:
- `id` UUID: 14.6 bytes (no compression, 1:1 ratio)
- `tags` Map: 9.6 bytes (16x compression)
- `min_time`/`max_time`: ~1 byte each

### Solution: Improve ID storage in Tags table

The `id` column in tags table has 1:1 compression ratio (no benefit from ZSTD).
Consider:
1. Using a simpler integer ID instead of UUID
2. Auto-increment ID with UUID→ID mapping table
3. Separate lookup table for series metadata

---

## Implementation Roadmap

### Phase 1: Foundation (1-2 weeks)
- [ ] Create `BitWriter`/`BitReader` utilities
- [ ] Unit tests for bit-level I/O

### Phase 2: Block-Level Timestamps (2-3 weeks)
- [ ] Implement `CompressionCodecBlockDoubleDelta`
- [ ] Register codec
- [ ] Add tests
- [ ] Benchmark against DoubleDeltaVarInt

### Phase 3: Block-Level Values (2-3 weeks)
- [ ] Implement `CompressionCodecBlockGorilla`
- [ ] Register codec
- [ ] Add tests
- [ ] Benchmark against GorillaV2

### Phase 4: Series ID Optimization (3-4 weeks)
- [ ] Design block-level series ID storage
- [ ] Implement `CompressionCodecSeriesBlock`
- [ ] Coordinate with MergeTree ordering

### Phase 5: Integration & Testing (2 weeks)
- [ ] End-to-end testing with TimeSeries engine
- [ ] Performance benchmarks
- [ ] Documentation

---

## Backwards Compatibility

All new codecs should be registered as NEW codec names:
- `BlockDoubleDelta` (new) vs `DoubleDeltaVarInt` (existing)
- `BlockGorilla` (new) vs `GorillaV2` (existing)
- `SeriesBlock` (new)

This ensures existing tables continue to work.

---

## Expected Final Results

| Column | Current | With Block Codecs | Improvement |
|--------|---------|-------------------|-------------|
| timestamp | 0.62 B | 0.15 B | **4.1x** |
| value | 0.51 B | 0.25 B | **2.0x** |
| id | 0.17 B | 0.05 B | **3.4x** |
| **TOTAL** | **1.57 B** | **0.45 B** | **3.5x** |

**Target: 0.45 bytes/sample** - Competitive with VictoriaMetrics (0.4 B)!

---

## References

- [Facebook Gorilla Paper](https://www.vldb.org/pvldb/vol8/p1816-teller.pdf)
- [VictoriaMetrics Architecture](https://docs.victoriametrics.com/Single-server-VictoriaMetrics.html#storage)
- [ClickHouse Compression Codecs](https://clickhouse.com/docs/en/sql-reference/statements/create/table#column-compression-codecs)
