-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA FOR PROMETHEUS REMOTE WRITE
-- Using TimeSeries engine with optimized inner table configuration
-- Target: 20M+ active series, high ingest, 3-day retention
-- 
-- This creates ONLY the metrics TimeSeries table, which automatically
-- creates optimized inner tables (.inner_id.data.*, .inner_id.tags.*, 
-- .inner_id.metrics.*) with our specified codecs and settings.
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- Enable experimental features for the session
SET allow_experimental_time_series_table = 1;

-- Drop existing tables if any
DROP TABLE IF EXISTS otel.metrics;

-- =====================================================================
-- CREATE TIMESERIES TABLE WITH OPTIMIZED INNER TABLE CONFIGURATION
-- 
-- Column definitions here are inherited by the inner tables.
-- ENGINE specifications for DATA/TAGS/METRICS configure the inner tables.
-- =====================================================================

CREATE TABLE otel.metrics
(
    -- Data table columns (with optimized codecs for compression)
    `id` UUID DEFAULT reinterpretAsUUID(sipHash128(metric_name, all_tags)) CODEC(ZSTD(1)),
    `timestamp` DateTime64(3) CODEC(Delta(8), ZSTD(1)),   -- Delta for sequential timestamps
    `value` Float64 CODEC(Gorilla, ZSTD(1)),              -- Gorilla for time-series floats
    
    -- Tags table columns (with optimized codecs)
    `metric_name` LowCardinality(String) CODEC(ZSTD(1)),
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(1)),
    `all_tags` Map(String, String),  -- EPHEMERAL is automatic for TimeSeries
    `min_time` Nullable(DateTime64(3)) CODEC(Delta(8), ZSTD(1)),  -- Delta for time bounds
    `max_time` Nullable(DateTime64(3)) CODEC(Delta(8), ZSTD(1)),  -- Delta for time bounds
    
    -- Metrics table columns (with codecs)
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
-- VERIFICATION QUERIES (run these to verify the setup)
-- =====================================================================
-- 
-- Show the created table structure:
--   SHOW CREATE TABLE otel.metrics;
--
-- List all tables in otel database:
--   SELECT name, engine FROM system.tables WHERE database = 'otel';
--
-- Describe inner tables:
--   SELECT name FROM system.tables WHERE database = 'otel' AND name LIKE '.inner%';
--

