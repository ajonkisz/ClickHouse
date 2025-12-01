-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA FOR PROMETHEUS REMOTE WRITE
-- Target: 20M+ active series, high ingest, 3-day retention
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- Enable experimental features for the session
SET allow_experimental_time_series_table = 1;

-- =====================================================================
-- 1. DATA TABLE (The Heavy Lifter)
-- Stores the raw time-series points.
-- =====================================================================
DROP TABLE IF EXISTS otel.ts_data;
CREATE TABLE otel.ts_data
(
    `id` UUID CODEC(ZSTD(1)),                           -- Explicit codec for UUID compression
    `timestamp` DateTime64(3) CODEC(Delta(8), ZSTD(1)), -- Delta for sequential timestamps
    `value` Float64 CODEC(Gorilla, ZSTD(1))             -- Gorilla for time-series floats
)
ENGINE = MergeTree
PARTITION BY toDate(timestamp) -- Daily partitions for efficient TTL
ORDER BY (id, timestamp)       -- Physical sort order: Series ID -> Time
TTL timestamp + INTERVAL 3 DAY DELETE
SETTINGS
    index_granularity = 8192,
    ttl_only_drop_parts = 1,
    merge_with_ttl_timeout = 3600,
    parts_to_throw_insert = 3000,
    parts_to_delay_insert = 500,
    max_parts_in_total = 100000,
    max_suspicious_broken_parts = 5;

-- =====================================================================
-- 2. TAGS TABLE (The Index)
-- Stores the mapping from "Tags" -> "Series ID".
-- =====================================================================
DROP TABLE IF EXISTS otel.ts_tags;
CREATE TABLE otel.ts_tags
(
    -- 1. Auto-generate ID from hash of metric name + all tags
    `id` UUID DEFAULT reinterpretAsUUID(sipHash128(metric_name, all_tags)) CODEC(ZSTD(3)),
    
    `metric_name` LowCardinality(String) CODEC(ZSTD(1)),
    
    -- 2. Store identifying tags as a Map
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(1)),
    
    -- 3. EPHEMERAL: Used for ID generation but NOT stored (Saves disk space)
    `all_tags` Map(String, String) EPHEMERAL,
    
    -- 4. Time bounds with Delta compression for sequential timestamps
    `min_time` SimpleAggregateFunction(min, Nullable(DateTime64(3))) CODEC(Delta(8), ZSTD(1)),
    `max_time` SimpleAggregateFunction(max, Nullable(DateTime64(3))) CODEC(Delta(8), ZSTD(1)),

    -- 5. Bloom Filter Indices for fast Label filtering
    INDEX idx_tags_key mapKeys(tags) TYPE bloom_filter(0.01) GRANULARITY 1,
    INDEX idx_tags_value mapValues(tags) TYPE tokenbf_v1(131072, 3, 0) GRANULARITY 4,
    INDEX idx_metric_name metric_name TYPE ngrambf_v1(3, 131072, 3, 0) GRANULARITY 1
)
ENGINE = AggregatingMergeTree
PRIMARY KEY metric_name
ORDER BY (metric_name, id)
SETTINGS
    index_granularity = 8192;

-- =====================================================================
-- 3. METRICS META TABLE (The Catalog)
-- Stores metadata like Help text, Units, and Types.
-- =====================================================================
DROP TABLE IF EXISTS otel.ts_metrics;
CREATE TABLE otel.ts_metrics
(
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(1)),
    `type` LowCardinality(String) CODEC(ZSTD(1)),
    `unit` LowCardinality(String) CODEC(ZSTD(1)),
    `help` String CODEC(ZSTD(1))
)
ENGINE = ReplacingMergeTree
ORDER BY metric_family_name;

-- =====================================================================
-- 4. THE CONTROLLER TABLE
-- This Virtual Table links the 3 physical tables above.
-- Writes to this table are automatically routed to the underlying tables.
-- =====================================================================
DROP TABLE IF EXISTS otel.metrics;
CREATE TABLE otel.metrics
ENGINE = TimeSeries(otel.ts_data, otel.ts_tags, otel.ts_metrics);

