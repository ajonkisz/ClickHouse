-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA V5 - EXPLICIT TABLES + LOWCARDINALITY
-- Uses explicit ts_data, ts_tags, ts_metrics tables (not inner tables)
-- =====================================================================
--
-- FIXED: LowCardinality now supported on ALL external tables!
-- The artificial restriction in StorageTimeSeries.cpp has been removed.
--
-- TABLES CREATED:
-- =====================================================================
-- | Table      | Purpose                        | Engine              |
-- |------------|--------------------------------|---------------------|
-- | ts_data    | Time-series samples (id,ts,v)  | MergeTree           |
-- | ts_tags    | Series metadata (id,name,tags) | AggregatingMergeTree|
-- | ts_metrics | Metric family metadata         | ReplacingMergeTree  |
-- | metrics_v5 | TimeSeries view over above     | TimeSeries          |
-- =====================================================================
--
-- INDEX ON TAGS:
-- =====================================================================
-- idx_tags_values uses ngrambf_v1(3, 8192, 2, 0) for fast substring/regex
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

SET allow_experimental_time_series_table = 1;

DROP TABLE IF EXISTS otel.metrics_v5;
DROP TABLE IF EXISTS otel.ts_data;
DROP TABLE IF EXISTS otel.ts_tags;
DROP TABLE IF EXISTS otel.ts_metrics;

-- =====================================================================
-- 1. DATA TABLE: Stores actual time-series samples
-- =====================================================================

CREATE TABLE otel.ts_data
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
    index_granularity = 8192,
    ttl_only_drop_parts = 1,
    merge_with_ttl_timeout = 3600,
    parts_to_throw_insert = 3000,
    parts_to_delay_insert = 500,
    max_parts_in_total = 100000,
    max_suspicious_broken_parts = 5;

-- =====================================================================
-- 2. TAGS TABLE: WITH LowCardinality + ngram indexes
-- =====================================================================

CREATE TABLE otel.ts_tags
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
-- 3. METRICS TABLE: WITH LowCardinality (now supported!)
-- =====================================================================

CREATE TABLE otel.ts_metrics
(
    `metric_family_name` LowCardinality(String) CODEC(ZSTD(3)),
    `type` LowCardinality(String) CODEC(ZSTD(3)),
    `unit` LowCardinality(String) CODEC(ZSTD(3)),
    `help` String CODEC(ZSTD(3))
)
ENGINE = ReplacingMergeTree
ORDER BY metric_family_name;

-- =====================================================================
-- 4. TIMESERIES VIEW: With LowCardinality everywhere
-- =====================================================================

CREATE TABLE otel.metrics_v5
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
DATA otel.ts_data
TAGS otel.ts_tags
METRICS otel.ts_metrics;

-- =====================================================================
-- VERIFICATION
-- =====================================================================
--
-- All tables now support LowCardinality:
--   SELECT name, type FROM system.columns WHERE database = 'otel' AND table IN ('ts_tags', 'ts_metrics');
-- =====================================================================
