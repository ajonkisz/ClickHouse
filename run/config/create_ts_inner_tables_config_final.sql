-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA - FINAL RECOMMENDATION
-- =====================================================================
--
-- Based on extensive testing with 769M samples (V6-V9 comparison)
--
-- KEY FINDINGS:
-- =====================================================================
--
-- 1. GRANULARITY: 64K is CRITICAL (non-negotiable)
--    - 64K: 0.43 B/sample
--    - 8K:  1.62 B/sample (4x WORSE!)
--    - DoubleDelta encoding resets at granule boundaries
--    - Larger granules = fewer resets = better timestamp compression
--
-- 2. ZSTD LEVEL: Use level 1 (same compression, faster)
--    - ZSTD(1): 0.4296 B/sample
--    - ZSTD(3): 0.4289 B/sample
--    - Difference: only 0.2%
--    - ZSTD(1) is faster for both compression and decompression
--
-- 3. CODEC COMBINATION:
--    - ID (UUID): ZSTD(1) - achieves 2733x compression
--    - Timestamp: DoubleDeltaVarInt + ZSTD(1) - achieves 40x compression
--    - Value: GorillaV2 + ZSTD(1) - achieves 36x compression
--
-- COMPRESSION RESULTS (769M samples):
-- =====================================================================
--
-- | Column    | B/sample | Compression Ratio |
-- |-----------|----------|-------------------|
-- | id        | 0.006    | 2733x             |
-- | timestamp | 0.201    | 40x               |
-- | value     | 0.222    | 36x               |
-- | TOTAL     | 0.429    | ~37x              |
--
-- COMPARISON WITH VICTORIAMETRICS:
-- - ClickHouse: 0.43 B/sample
-- - VictoriaMetrics: 1.72 B/sample
-- - ClickHouse is 4x more storage efficient
--
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- =====================================================================
-- 1. DATA TABLE - Optimized for storage
-- =====================================================================

CREATE TABLE IF NOT EXISTS otel.ts_data
(
    -- UUID for series identification
    -- ZSTD(1) achieves 2733x compression on UUIDs
    `id` UUID CODEC(ZSTD(1)),
    
    -- Millisecond-precision timestamps
    -- DoubleDeltaVarInt is perfect for regular intervals (~2.3s in our case)
    -- Achieves 40x compression with 64K granularity
    `timestamp` DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(1)),
    
    -- Float64 metric values
    -- GorillaV2 uses XOR encoding optimized for similar consecutive values
    -- Achieves 36x compression
    `value` Float64 CODEC(GorillaV2, ZSTD(1))
)
ENGINE = MergeTree
PARTITION BY toDate(timestamp)
ORDER BY (id, timestamp)
TTL timestamp + INTERVAL 3 DAY DELETE
SETTINGS
    -- CRITICAL: 64K granularity for DoubleDelta efficiency
    -- Smaller values (8K) result in 4x worse compression!
    index_granularity = 65536,
    
    -- TTL optimization
    ttl_only_drop_parts = 1,
    merge_with_ttl_timeout = 3600,
    
    -- Part management
    parts_to_throw_insert = 3000,
    parts_to_delay_insert = 500,
    max_parts_in_total = 100000;

-- =====================================================================
-- 2. TAGS TABLE - Series metadata
-- =====================================================================

CREATE TABLE IF NOT EXISTS otel.ts_tags
(
    `id` UUID,
    
    -- LowCardinality for efficient storage of repeated metric names
    `metric_name` LowCardinality(String) CODEC(ZSTD(1)),
    
    -- Map for flexible label storage
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(1)),
    
    -- Time range tracking for efficient queries
    `min_time` SimpleAggregateFunction(min, Nullable(DateTime64(3))),
    `max_time` SimpleAggregateFunction(max, Nullable(DateTime64(3))),
    
    -- Indexes for fast label queries
    INDEX idx_tags_values mapValues(tags) TYPE ngrambf_v1(3, 8192, 2, 0) GRANULARITY 4,
    INDEX idx_tags_keys mapKeys(tags) TYPE bloom_filter GRANULARITY 4
)
ENGINE = AggregatingMergeTree
PRIMARY KEY metric_name
ORDER BY (metric_name, id)
SETTINGS 
    -- Smaller granularity OK for tags (small table)
    index_granularity = 4096;

-- =====================================================================
-- 3. METRICS TABLE - Metric family metadata
-- =====================================================================

CREATE TABLE IF NOT EXISTS otel.ts_metrics
(
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(1)),
    `type` LowCardinality(String) CODEC(ZSTD(1)),
    `unit` LowCardinality(String) CODEC(ZSTD(1)),
    `help` String CODEC(ZSTD(1))
)
ENGINE = ReplacingMergeTree
ORDER BY metric_family_name;

-- =====================================================================
-- 4. TIMESERIES TABLE - Unified view with Prometheus API support
-- =====================================================================

SET allow_experimental_time_series_table = 1;

CREATE TABLE IF NOT EXISTS otel.metrics
(
    `id` UUID DEFAULT reinterpretAsUUID(sipHash128(metric_name, all_tags)) CODEC(ZSTD(1)),
    `timestamp` DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(1)),
    `value` Float64 CODEC(GorillaV2, ZSTD(1)),
    `metric_name` LowCardinality(String) CODEC(ZSTD(1)),
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(1)),
    `all_tags` Map(String, String) EPHEMERAL,
    `min_time` Nullable(DateTime64(3)),
    `max_time` Nullable(DateTime64(3)),
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(1)),
    `type` LowCardinality(String) CODEC(ZSTD(1)),
    `unit` LowCardinality(String) CODEC(ZSTD(1)),
    `help` String CODEC(ZSTD(1))
)
ENGINE = TimeSeries
SETTINGS
    store_min_time_and_max_time = 1,
    aggregate_min_time_and_max_time = 1,
    filter_by_min_time_and_max_time = 1,
    use_all_tags_column_to_generate_id = 1
DATA otel.ts_data
TAGS otel.ts_tags
METRICS otel.ts_metrics;

-- =====================================================================
-- TRADE-OFFS TO BE AWARE OF:
-- =====================================================================
--
-- 1. STORAGE vs QUERY SPEED
--    - 64K granularity = excellent compression
--    - But point queries must read entire 64K-row granules
--    - For query-heavy workloads, consider 8K granularity (4x more storage)
--
-- 2. TTL BEHAVIOR
--    - Parts only dropped when ALL data in part is expired
--    - Use ttl_only_drop_parts = 1 for predictable behavior
--
-- 3. PROMETHEUS COMPATIBILITY
--    - PromQL queries work but are slower than native VM
--    - CH scans more rows due to MergeTree architecture
--    - VM: ~100ms queries, CH: ~1000ms queries (10x slower)
--
-- =====================================================================
-- TEST RESULTS SUMMARY (769M samples, 370 minutes of data):
-- =====================================================================
--
-- | Schema Configuration | B/sample | Compression |
-- |---------------------|----------|-------------|
-- | 8K granularity      | 1.62     | 10x         |
-- | 64K + ZSTD(3)       | 0.43     | 37x         |
-- | 64K + ZSTD(1)       | 0.43     | 37x         | <- RECOMMENDED
-- | VictoriaMetrics     | 1.72     | 9x          |
--
-- =====================================================================
