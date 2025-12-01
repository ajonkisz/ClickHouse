-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA FOR PROMETHEUS REMOTE WRITE - V3 (OPTIMIZED)
-- Using TimeSeries engine with BEST compression codecs
-- 
-- REQUIRES: ClickHouse with DoubleDeltaVarInt and GorillaV2 codecs
-- 
-- Target: 20M+ active series, high ingest, 3-day retention
-- 
-- V3 IMPROVEMENTS OVER V2:
-- =====================================================================
-- | Change                  | Impact                                   |
-- |-------------------------|------------------------------------------|
-- | UInt64 for id           | 0.089 → 0.051 B/row (43% smaller)        |
-- | ZSTD(3) instead of (1)  | ~2-5% better compression                 |
-- =====================================================================
--
-- COMPRESSION TEST RESULTS (10M rows, optimized):
-- =====================================================================
-- | Column    | Codec                          | Bytes/Row | Ratio    |
-- |-----------|--------------------------------|-----------|----------|
-- | id        | UInt64 + ZSTD(3)               | 0.051 B   | 157x     |
-- | timestamp | DoubleDeltaVarInt + ZSTD(3)    | 0.875 B   | 9.2x     |
-- | value     | GorillaV2 + ZSTD(3)            | 0.475 B   | 16.8x    |
-- |-----------|--------------------------------|-----------|----------|
-- | TOTAL     |                                | 1.40 B    | 17.1x    |
-- =====================================================================
--
-- NOTE: Timestamp compression is limited by irregular scrape intervals.
-- With regular 15s intervals, timestamp would achieve ~0.1-0.2 B/row.
-- Current test data has 2-20ms jitter between samples.
--
-- COMPARISON:
-- =====================================================================
-- | Version | Bytes/Sample | Notes                                    |
-- |---------|--------------|------------------------------------------|
-- | V1      | ~2.3 B       | UUID + Delta + Gorilla                   |
-- | V2      | ~1.6 B       | UUID + DoubleDeltaVarInt + GorillaV2     |
-- | V3      | ~1.4 B       | UInt64 + DoubleDeltaVarInt + GorillaV2   |
-- | VM      | ~0.4 B       | Block-level bit-packing (theoretical)   |
-- =====================================================================
--
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- Enable experimental features for the session
SET allow_experimental_time_series_table = 1;

-- Drop existing tables if any
DROP TABLE IF EXISTS otel.metrics_v3;

-- =====================================================================
-- CREATE TIMESERIES TABLE WITH V3 OPTIMIZED CODECS
-- =====================================================================

CREATE TABLE otel.metrics_v3
(
    -- =================================================================
    -- DATA TABLE COLUMNS
    -- =================================================================
    
    -- Series ID: UInt64 computed from metric name + labels using sipHash64
    -- 
    -- Why UInt64 instead of UUID:
    --   - UUID is 16 bytes, UInt64 is 8 bytes (50% smaller)
    --   - After ZSTD compression: UUID = 0.089 B/row, UInt64 = 0.051 B/row
    --   - sipHash64 has sufficient collision resistance for ~4B unique series
    --   - For >4B series, use UInt128 or UUID
    `id` UInt64 DEFAULT sipHash64(metric_name, all_tags) CODEC(ZSTD(3)),
    
    -- Timestamp: DoubleDeltaVarInt + ZSTD(3)
    -- 
    -- DoubleDeltaVarInt encoding:
    --   1. Compute delta: ts[n] - ts[n-1]
    --   2. Compute delta-of-delta: delta[n] - delta[n-1]
    --   3. Variable-length encode the delta-of-delta
    --
    -- ZSTD(3) provides additional compression of the varint stream
    --
    -- Performance depends heavily on scrape regularity:
    --   - Regular 15s scrapes: ~0.1-0.2 B/row (delta-of-delta ≈ 0)
    --   - Irregular scrapes: ~0.8-1.0 B/row (this test data)
    `timestamp` DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    
    -- Value: GorillaV2 + ZSTD(3)
    --
    -- GorillaV2 (enhanced Gorilla XOR) encoding:
    --   1. XOR current value with previous
    --   2. Track leading/trailing zeros pattern
    --   3. Encode efficiently based on XOR result
    --
    -- Typical compression: 15-20x for slowly-changing metric values
    `value` Float64 CODEC(GorillaV2, ZSTD(3)),
    
    -- =================================================================
    -- TAGS TABLE COLUMNS
    -- =================================================================
    
    -- Metric name: LowCardinality provides excellent compression
    `metric_name` LowCardinality(String) CODEC(ZSTD(3)),
    
    -- Labels map: LowCardinality keys compress well
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(3)),
    
    -- Ephemeral column for computing ID (not stored)
    `all_tags` Map(String, String),
    
    -- Time bounds for efficient PromQL queries
    `min_time` Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    `max_time` Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    
    -- =================================================================
    -- METRICS TABLE COLUMNS (metadata)
    -- =================================================================
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(3)),
    `type` LowCardinality(String) CODEC(ZSTD(3)),
    `unit` LowCardinality(String) CODEC(ZSTD(3)),
    `help` String CODEC(ZSTD(3))
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
-- VERIFICATION QUERIES
-- =====================================================================
-- 
-- Show the created table structure:
--   SHOW CREATE TABLE otel.metrics_v3;
--
-- Check compression per column (after inserting data and optimizing):
--   SELECT 
--       column,
--       formatReadableSize(sum(column_data_compressed_bytes)) as compressed,
--       round(sum(column_data_uncompressed_bytes) / 
--             nullIf(sum(column_data_compressed_bytes), 0), 2) as ratio,
--       round(sum(column_data_compressed_bytes) / sum(rows), 3) as bytes_per_row
--   FROM system.parts_columns
--   WHERE database = 'otel' AND table LIKE '.inner_id.data.%' AND active = 1
--   GROUP BY column
--   ORDER BY column;
--
-- Check total bytes per row:
--   SELECT 
--       round(sum(data_compressed_bytes) / sum(rows), 3) as bytes_per_row
--   FROM system.parts
--   WHERE database = 'otel' AND table LIKE '.inner_id.data.%' AND active = 1;
--
-- =====================================================================

