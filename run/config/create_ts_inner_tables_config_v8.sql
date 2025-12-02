-- =====================================================================
-- CLICKHOUSE TIMESERIES SCHEMA V8 - ZSTD(1) TEST
-- =====================================================================
--
-- PURPOSE: Test impact of ZSTD(1) vs ZSTD(3)
-- CHANGE FROM V6: ZSTD(1) instead of ZSTD(3)
-- RESULT: Only 5.5% larger (0.430 vs 0.407 B/sample)
--
-- =====================================================================

CREATE DATABASE IF NOT EXISTS otel;

-- Data table with ZSTD(1)
CREATE TABLE otel.ts_data_v8
(
    id UUID CODEC(ZSTD(1)),
    timestamp DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(1)),
    value Float64 CODEC(GorillaV2, ZSTD(1))
)
ENGINE = MergeTree
PARTITION BY toDate(timestamp)
ORDER BY (id, timestamp)
TTL timestamp + INTERVAL 3 DAY DELETE
SETTINGS index_granularity = 65536;

-- Tags table with ZSTD(1)
CREATE TABLE otel.ts_tags_v8
(
    id UUID,
    metric_name LowCardinality(String) CODEC(ZSTD(1)),
    tags Map(LowCardinality(String), String) CODEC(ZSTD(1)),
    min_time SimpleAggregateFunction(min, Nullable(DateTime64(3))),
    max_time SimpleAggregateFunction(max, Nullable(DateTime64(3))),
    INDEX idx_tags_values mapValues(tags) TYPE ngrambf_v1(3, 8192, 2, 0) GRANULARITY 4,
    INDEX idx_tags_keys mapKeys(tags) TYPE bloom_filter GRANULARITY 4
)
ENGINE = AggregatingMergeTree
PRIMARY KEY metric_name
ORDER BY (metric_name, id)
SETTINGS index_granularity = 4096;

