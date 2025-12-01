🚀 What ClickHouse TimeSeries Needs to Match/Beat VictoriaMetrics
Current Storage Efficiency Gap
Metric	VictoriaMetrics	ClickHouse TimeSeries	Gap
Bytes/sample	0.4-1.5	2.8-3.0	2-7x
Compression ratio	20-80x	11-15x	~3x
🔧 HIGH IMPACT Improvements (Worth Contributing)
1. Remove UUID Per-Sample Overhead
Current: Each sample stores a 16-byte UUID linking to the tags table
Problem: Even with 88x compression, it's still ~0.18 bytes/sample overhead
Solution:
-- Instead of storing UUID per sample:-- Current: (id UUID, timestamp DateTime64, value Float64)-- Store metric_id as varint:-- Proposed: (metric_id UInt32, timestamp DateTime64, value Float64)-- Or: Store metric_id once per block, not per row
Impact: Could save 0.1-0.3 bytes/sample
2. Implement Delta-of-Delta for Timestamps
Current: Delta encoding (5x compression)
VictoriaMetrics: Delta-of-delta + variable-length encoding (20-50x)
Implementation:
// Current: store delta from previous timestampint64_t delta = timestamp - prev_timestamp;// Proposed: store delta of deltaint64_t delta_of_delta = delta - prev_delta;// Most values will be 0 (constant interval) → 1 bit// Occasional jitter → 8-16 bits
Impact: Could improve timestamp compression from 5x to 15-20x
3. Gorilla XOR Optimization
Current: Basic Gorilla implementation (12-15x)
VictoriaMetrics: Enhanced with leading/trailing zero optimization
Implementation:
// Current: XOR with previous value, encode resultuint64_t xor_val = current_bits ^ prev_bits;// Proposed: Track leading zeros and trailing zeros// If similar to previous XOR, encode only the meaningful bits// Use Huffman-like encoding for common patterns
Impact: Could improve value compression from 15x to 25-40x
4. Series ID Deduplication in Data Blocks
Current: UUID stored per row
Proposed: Block-level series mapping
// Block header stores: [series_id_1, series_id_2, ...]// Each row stores: [series_index (1-2 bytes), timestamp_delta, value]// Instead of: [uuid (16 bytes), timestamp, value]
Impact: Massive savings for high-cardinality data
🔧 MEDIUM IMPACT Improvements
5. Native Downsampling Engine
-- Automatic rollup tablesCREATE TABLE metrics_1m ENGINE = AggregatingMergeTreeAS SELECT     id,     toStartOfMinute(timestamp) as ts,    avgState(value) as avg_val,    minState(value) as min_val,    maxState(value) as max_valFROM metricsGROUP BY id, ts;
VictoriaMetrics has: -downsampling.period flag for automatic downsampling
6. Streaming Deduplication
Current: ReplacingMergeTree dedupes at merge time
Proposed: Bloom filter at insert time to reject duplicates
// At insert:if (bloom_filter.contains(metric_id, timestamp)) {    skip_insert();}
7. Inverted Index for Labels
Current: Tags stored as Map(String, String) per series
Proposed: Separate inverted index like VictoriaMetrics
-- label_to_series indexCREATE TABLE label_index (    label_name LowCardinality(String),    label_value String,    series_ids Array(UInt64)  -- Bitmap would be even better) ENGINE = MergeTree ORDER BY (label_name, label_value);
🔧 CRITICAL PromQL Improvements
8. Complete PromQL Implementation
Currently missing/broken:
-- These fail with "not implemented":avg(metric)                    -- AggregationOperatorsum by (label)(metric)         -- Groupingrate(metric[5m])              -- Range vectors  histogram_quantile(0.99, ...)  -- Histogram functions
Priority fixes:
AggregationOperator (avg, sum, count, min, max)
by / without grouping
Range vector functions (rate, increase, delta)
Subquery support
9. Fix ISO 8601 Timestamp Parsing
Already fixed in proxy, but should be in ClickHouse core:
// In PromQL handler, before lookback_delta calculation:if (is_iso8601(timestamp_str)) {    timestamp = parse_iso8601_to_unix(timestamp_str);}
📊 Projected Impact
Improvement	Current	Projected	Effort
Remove UUID overhead	2.8 B/s	2.5 B/s	Medium
Delta-of-delta timestamps	2.5 B/s	2.0 B/s	High
Enhanced Gorilla	2.0 B/s	1.5 B/s	High
Block-level series ID	1.5 B/s	1.0 B/s	High
Combined	2.8 B/s	0.8-1.2 B/s	-
This would put ClickHouse on par with VictoriaMetrics for storage efficiency!
🎯 Recommended Contribution Strategy
PHASE 1 (Quick Wins - 1-2 months)├── Fix ISO 8601 parsing in PromQL handler├── Implement basic aggregation operators (avg, sum, count)└── Add rate() function supportPHASE 2 (Storage Optimization - 3-6 months)├── Delta-of-delta timestamp encoding├── Enhanced Gorilla for Float64└── Block-level series ID compressionPHASE 3 (Architecture - 6-12 months)├── Inverted label index├── Native downsampling└── Streaming deduplication
Where to Contribute
ClickHouse Core (C++): https://github.com/ClickHouse/ClickHouse
src/Storages/TimeSeries/ - TimeSeries engine
src/Processors/Formats/Impl/PrometheusTextOutputFormat.cpp - PromQL
ClickHouse Docs: Fix documentation gaps
Test Cases: Add PromQL compatibility tests
Summary
To match VictoriaMetrics (0.8-1.5 bytes/sample):
✅ Most impactful: Delta-of-delta timestamps + enhanced Gorilla
✅ Quick win: Remove per-sample UUID overhead
✅ Critical for adoption: Complete PromQL implementation
ClickHouse's unique advantages to leverage:
SQL joins with logs/traces (unified observability)
Existing MergeTree optimization expertise
SharedMergeTree cloud-native architecture
Massive community and enterprise backing
With these improvements, ClickHouse could be the unified observability platform that handles metrics as efficiently as VictoriaMetrics while offering SQL power that VictoriaMetrics can't match.