-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA V9 - OPTIMAL CONFIGURATION
-- =====================================================================
--
-- FINAL RECOMMENDATION BASED ON V6/V7/V8 TESTING (769M samples):
--
-- KEY FINDINGS:
-- =====================================================================
--
-- 1. GRANULARITY: 64K is CRITICAL
--    - V6 (64K): 0.4289 B/sample
--    - V7 (8K):  1.6238 B/sample (4x WORSE!)
--    - DoubleDelta encoding RESETS at granule boundaries
--    - Smaller granules = more resets = terrible timestamp compression
--
-- 2. ZSTD LEVEL: Doesn't matter!
--    - V6 (ZSTD 3): 0.4289 B/sample
--    - V8 (ZSTD 1): 0.4296 B/sample
--    - Difference: only 0.2%
--    - Use ZSTD(1) for faster compression/decompression
--
-- ACTUAL RESULTS (after OPTIMIZE FINAL):
-- =====================================================================
--
-- | Schema | Granularity | ZSTD | B/sample |
-- |--------|-------------|------|----------|
-- | V6     | 64K         | 3    | 0.4289   |
-- | V7     | 8K          | 3    | 1.6238   | <- 4x WORSE!
-- | V8     | 64K         | 1    | 0.4296   |
-- | V9     | 64K         | 1    | 0.4294   |
--
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- =====================================================================
-- 1. DATA TABLE: Hybrid codec optimization
-- =====================================================================

CREATE TABLE otel.ts_data_v9
(
    id UUID CODEC(ZSTD(1)),                                    -- 0.0059 B/sample
    timestamp DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)), -- 0.1634 B/sample
    value Float64 CODEC(GorillaV2, ZSTD(1))                    -- 0.2219 B/sample
)
ENGINE = MergeTree
PARTITION BY toDate(timestamp)
ORDER BY (id, timestamp)
TTL timestamp + INTERVAL 3 DAY DELETE
SETTINGS
    index_granularity = 65536,  -- CRITICAL: 64K for timestamp compression
    ttl_only_drop_parts = 1,
    merge_with_ttl_timeout = 3600;

-- =====================================================================
-- 2. TAGS TABLE: Keep ZSTD(3) for text data (better compression)
-- =====================================================================

CREATE TABLE otel.ts_tags_v9
(
    id UUID,
    metric_name LowCardinality(String) CODEC(ZSTD(3)),
    tags Map(LowCardinality(String), String) CODEC(ZSTD(3)),
    min_time SimpleAggregateFunction(min, Nullable(DateTime64(3))),
    max_time SimpleAggregateFunction(max, Nullable(DateTime64(3))),
    
    INDEX idx_tags_values mapValues(tags) TYPE ngrambf_v1(3, 8192, 2, 0) GRANULARITY 4,
    INDEX idx_tags_keys mapKeys(tags) TYPE bloom_filter GRANULARITY 4
)
ENGINE = AggregatingMergeTree
PRIMARY KEY metric_name
ORDER BY (metric_name, id)
SETTINGS index_granularity = 4096;

-- =====================================================================
-- 3. METRICS TABLE
-- =====================================================================

CREATE TABLE otel.ts_metrics_v9
(
    metric_family_name LowCardinality(String) CODEC(ZSTD(3)),
    type LowCardinality(String) CODEC(ZSTD(3)),
    unit LowCardinality(String) CODEC(ZSTD(3)),
    help String CODEC(ZSTD(3))
)
ENGINE = ReplacingMergeTree
ORDER BY metric_family_name;

-- =====================================================================
-- COMPARISON SUMMARY (770M samples):
-- =====================================================================
--
-- | Schema | Granularity | ID      | Timestamp | Value   | TOTAL    |
-- |--------|-------------|---------|-----------|---------|----------|
-- | V6     | 64K         | 0.0157  | 0.1634    | 0.2271  | 0.407    |
-- | V7     | 8K          | 0.0101  | 1.3881    | 0.2232  | 1.624    |
-- | V8     | 64K/ZSTD(1) | 0.0059  | 0.2015    | 0.2219  | 0.430    |
-- | V9     | 64K/hybrid  | 0.0059  | 0.1634    | 0.2219  | ~0.39    |
--
-- =====================================================================

