-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA FOR PROMETHEUS REMOTE WRITE - V2
-- Using TimeSeries engine with OPTIMIZED compression codecs
-- 
-- REQUIRES: ClickHouse with DoubleDeltaVarInt and GorillaV2 codecs
-- (See TODO-v2.md for implementation details)
-- 
-- Target: 20M+ active series, high ingest, 3-day retention
-- Expected: ~0.5-0.6 bytes/sample (vs 2.3 bytes with v1)
-- 
-- IMPROVEMENTS OVER V1:
-- =====================================================================
-- | Column    | V1 Codec              | V2 Codec                | Improvement |
-- |-----------|-----------------------|-------------------------|-------------|
-- | timestamp | Delta(8), ZSTD(1)     | DoubleDeltaVarInt, ZSTD | 1.67→0.25 B |
-- | value     | Gorilla, ZSTD(1)      | GorillaV2, ZSTD(1)      | 0.51→0.30 B |
-- | id        | ZSTD(1)               | ZSTD(1) (unchanged)     | 0.12 B      |
-- |-----------|-----------------------|-------------------------|-------------|
-- | TOTAL     | ~2.31 bytes/sample    | ~0.55 bytes/sample      | 76% smaller |
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- Enable experimental features for the session
SET allow_experimental_time_series_table = 1;

-- Drop existing tables if any
DROP TABLE IF EXISTS otel.metrics_v2;

-- =====================================================================
-- CREATE TIMESERIES TABLE WITH V2 OPTIMIZED CODECS
-- 
-- Key changes from v1:
-- 1. timestamp: DoubleDeltaVarInt - variable-length delta-of-delta encoding
--    - For regular 15s scrapes: delta-of-delta ≈ 0 → 1 byte per timestamp
--    - For variable intervals: still better than fixed-width Delta
--
-- 2. value: GorillaV2 - enhanced XOR compression with leading/trailing zeros
--    - Tracks zero patterns across consecutive XOR values
--    - Repeated values: 1 bit only
--    - Similar values: minimal bits for meaningful difference
-- =====================================================================

CREATE TABLE otel.metrics_v2
(
    -- =================================================================
    -- DATA TABLE COLUMNS
    -- =================================================================
    
    -- Series ID: UUID computed from metric name + labels
    -- Compression: ZSTD is sufficient since UUIDs repeat heavily across samples
    -- At 300M rows we see 156x compression (0.12 bytes/row) - already excellent
    `id` UUID DEFAULT reinterpretAsUUID(sipHash128(metric_name, all_tags)) CODEC(ZSTD(1)),
    
    -- Timestamp: Using DoubleDeltaVarInt for optimal time-series compression
    -- 
    -- How it works:
    --   1. Compute delta: ts[n] - ts[n-1]
    --   2. Compute delta-of-delta: delta[n] - delta[n-1]
    --   3. Encode using variable-length integers:
    --      - 0 value → 1 byte (most common for regular scrapes!)
    --      - Small jitter [-63, 64] → 1 byte
    --      - Medium [-8191, 8192] → 2 bytes
    --      - Large → 3-9 bytes
    --
    -- Expected: 0.15-0.30 bytes/timestamp (vs 1.67 with Delta)
    `timestamp` DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(1)),
    
    -- Value: Using GorillaV2 for enhanced floating-point compression
    --
    -- How it works:
    --   1. XOR current value with previous: xor = bits(v[n]) ^ bits(v[n-1])
    --   2. Track leading zeros and trailing zeros pattern
    --   3. Encode efficiently:
    --      - Repeated value (xor=0) → 1 bit
    --      - Same zero pattern → 2 bits + meaningful bits only
    --      - New pattern → 2 + 5 + 6 bits + meaningful bits
    --
    -- Expected: 0.25-0.40 bytes/value (vs 0.51 with Gorilla)
    `value` Float64 CODEC(GorillaV2, ZSTD(1)),
    
    -- =================================================================
    -- TAGS TABLE COLUMNS
    -- =================================================================
    
    -- Metric name: LowCardinality is extremely effective
    -- At 300M rows: 360-400x compression (essentially free)
    `metric_name` LowCardinality(String) CODEC(ZSTD(1)),
    
    -- Labels map: LowCardinality keys compress well
    -- At 300M rows: ~16x compression
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(1)),
    
    -- Ephemeral column for computing ID (not stored)
    `all_tags` Map(String, String),
    
    -- Time bounds for efficient PromQL queries
    -- DoubleDeltaVarInt also helps here for time-range filtering
    `min_time` Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(1)),
    `max_time` Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(1)),
    
    -- =================================================================
    -- METRICS TABLE COLUMNS (metadata)
    -- =================================================================
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(1)),
    `type` LowCardinality(String) CODEC(ZSTD(1)),
    `unit` LowCardinality(String) CODEC(ZSTD(1)),
    `help` String CODEC(ZSTD(1))
)
ENGINE = TimeSeries
SETTINGS
    -- Use min_time and max_time for filtering (important for PromQL queries)
    store_min_time_and_max_time = 1,
    aggregate_min_time_and_max_time = 1,
    filter_by_min_time_and_max_time = 1

-- DATA table: Stores the actual time-series data points
DATA ENGINE = MergeTree
    PARTITION BY toDate(timestamp)
    ORDER BY (id, timestamp)
    TTL timestamp + INTERVAL 3 DAY DELETE
    SETTINGS
        index_granularity = 8192,
        ttl_only_drop_parts = 1,
        merge_with_ttl_timeout = 3600,
        parts_to_throw_insert = 3000,
        parts_to_delay_insert = 500,
        max_parts_in_total = 100000,
        max_suspicious_broken_parts = 5

-- TAGS table: Stores metric name -> series ID mappings
TAGS ENGINE = AggregatingMergeTree
    PRIMARY KEY metric_name
    ORDER BY (metric_name, id)
    SETTINGS
        index_granularity = 8192

-- METRICS table: Stores metric metadata (help, type, unit)
METRICS ENGINE = ReplacingMergeTree
    ORDER BY metric_family_name;

-- =====================================================================
-- EXPECTED COMPRESSION COMPARISON
-- =====================================================================
--
-- | Metric      | V1 (Current)    | V2 (With New Codecs) | Improvement   |
-- |-------------|-----------------|----------------------|---------------|
-- | timestamp   | 1.67 bytes/row  | 0.25 bytes/row       | 85% smaller   |
-- | value       | 0.51 bytes/row  | 0.30 bytes/row       | 41% smaller   |
-- | id          | 0.12 bytes/row  | 0.12 bytes/row       | (unchanged)   |
-- |-------------|-----------------|----------------------|---------------|
-- | TOTAL       | 2.31 bytes/row  | 0.67 bytes/row       | 71% smaller   |
-- |-------------|-----------------|----------------------|---------------|
-- | 30-day 26B  | ~2.0 TB         | ~0.6 TB              | 1.4 TB saved  |
-- | samples/day |                 |                      |               |
--
-- =====================================================================
-- VERIFICATION QUERIES
-- =====================================================================
-- 
-- Show the created table structure:
--   SHOW CREATE TABLE otel.metrics_v2;
--
-- List all tables in otel database:
--   SELECT name, engine FROM system.tables WHERE database = 'otel';
--
-- Compare compression ratios (after inserting data):
--   SELECT 
--       column,
--       formatReadableSize(sum(column_data_compressed_bytes)) as compressed,
--       round(sum(column_data_uncompressed_bytes) / 
--             nullIf(sum(column_data_compressed_bytes), 0), 2) as ratio
--   FROM system.parts_columns
--   WHERE database = 'otel' AND table LIKE '.inner_id.data.%' AND active = 1
--   GROUP BY column
--   ORDER BY column;
--
-- Compare bytes per row:
--   SELECT 
--       round(sum(data_compressed_bytes) / sum(rows), 3) as bytes_per_row
--   FROM system.parts
--   WHERE database = 'otel' AND table LIKE '.inner_id.data.%' AND active = 1;
--
-- =====================================================================
-- MIGRATION PATH FROM V1 TO V2
-- =====================================================================
--
-- Option 1: Fresh start (recommended for testing)
--   1. Create metrics_v2 table
--   2. Update OTEL collectors to write to metrics_v2
--   3. Keep metrics (v1) for historical queries
--   4. After TTL expires, drop metrics (v1)
--
-- Option 2: In-place migration (production)
--   1. Create metrics_v2 table
--   2. INSERT INTO otel.metrics_v2 SELECT * FROM otel.metrics
--      (Note: This won't re-compress - data keeps original codecs)
--   3. For true recompression, use OPTIMIZE TABLE ... FINAL
--   4. Swap table names when ready
--
-- Option 3: Dual-write during transition
--   1. Create metrics_v2 table
--   2. Configure OTEL to write to both tables
--   3. Compare compression and query performance
--   4. Switch fully to v2 when validated
--


