-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA V7 - 8K GRANULARITY (Query Optimized)
-- =====================================================================
--
-- PURPOSE: Compare query performance vs storage with 8K granularity
-- CHANGE FROM V6: index_granularity = 8192 (was 65536)
-- HYPOTHESIS: Faster queries at cost of ~38% more storage
--
-- DATA PERIOD: 2025-12-02 01:50:50 to 08:00:17 (~370 minutes)
-- SAMPLES: 769.5 million
--
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

SET allow_experimental_time_series_table = 1;

DROP TABLE IF EXISTS otel.metrics_v7;
DROP TABLE IF EXISTS otel.ts_data_v7;
DROP TABLE IF EXISTS otel.ts_tags_v7;
DROP TABLE IF EXISTS otel.ts_metrics_v7;

-- =====================================================================
-- 1. DATA TABLE: 8K granularity for faster queries
-- =====================================================================

CREATE TABLE otel.ts_data_v7
(
    `id` UUID CODEC(ZSTD(3)),
    `timestamp` DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    `value` Float64 CODEC(GorillaV2, ZSTD(3))
)
ENGINE = MergeTree
PARTITION BY toDate(timestamp)
ORDER BY (id, timestamp)
TTL timestamp + INTERVAL 3 DAY DELETE
SETTINGS
    -- KEY CHANGE: 8K granularity for faster queries
    index_granularity = 8192,
    ttl_only_drop_parts = 1,
    merge_with_ttl_timeout = 3600,
    parts_to_throw_insert = 3000,
    parts_to_delay_insert = 500,
    max_parts_in_total = 100000;

-- =====================================================================
-- 2. TAGS TABLE: Same as V6
-- =====================================================================

CREATE TABLE otel.ts_tags_v7
(
    `id` UUID,
    `metric_name` LowCardinality(String) CODEC(ZSTD(3)),
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(3)),
    `min_time` SimpleAggregateFunction(min, Nullable(DateTime64(3))),
    `max_time` SimpleAggregateFunction(max, Nullable(DateTime64(3))),
    
    INDEX idx_tags_values mapValues(tags) TYPE ngrambf_v1(3, 8192, 2, 0) GRANULARITY 4,
    INDEX idx_tags_keys mapKeys(tags) TYPE bloom_filter GRANULARITY 4
)
ENGINE = AggregatingMergeTree
PRIMARY KEY metric_name
ORDER BY (metric_name, id)
SETTINGS
    index_granularity = 4096;

-- =====================================================================
-- 3. METRICS TABLE: Same as V6
-- =====================================================================

CREATE TABLE otel.ts_metrics_v7
(
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(3)),
    `type` LowCardinality(String) CODEC(ZSTD(3)),
    `unit` LowCardinality(String) CODEC(ZSTD(3)),
    `help` String CODEC(ZSTD(3))
)
ENGINE = ReplacingMergeTree
ORDER BY metric_family_name;

-- =====================================================================
-- 4. TIMESERIES TABLE
-- =====================================================================

CREATE TABLE otel.metrics_v7
(
    `id` UUID DEFAULT reinterpretAsUUID(sipHash128(metric_name, all_tags)) CODEC(ZSTD(3)),
    `timestamp` DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    `value` Float64 CODEC(GorillaV2, ZSTD(3)),
    
    `metric_name` LowCardinality(String) CODEC(ZSTD(3)),
    `tags` Map(LowCardinality(String), String) CODEC(ZSTD(3)),
    `all_tags` Map(String, String) EPHEMERAL,
    `min_time` Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    `max_time` Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(3)),
    
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(3)),
    `type` LowCardinality(String) CODEC(ZSTD(3)),
    `unit` LowCardinality(String) CODEC(ZSTD(3)),
    `help` String CODEC(ZSTD(3))
)
ENGINE = TimeSeries
SETTINGS
    store_min_time_and_max_time = 1,
    aggregate_min_time_and_max_time = 1,
    filter_by_min_time_and_max_time = 1,
    use_all_tags_column_to_generate_id = 1
DATA otel.ts_data_v7
TAGS otel.ts_tags_v7
METRICS otel.ts_metrics_v7;

-- =====================================================================
-- EXPECTED IMPACT:
-- =====================================================================
--
-- | Metric | V6 (64K) | V7 (8K) | Change |
-- |--------|----------|---------|--------|
-- | Granularity | 65536 | 8192 | 8x smaller |
-- | Compression | Better | Worse | ~38% more storage |
-- | Query speed | Slower | Faster | Less rows per granule |
--
-- =====================================================================

