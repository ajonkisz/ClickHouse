-- On-prem SharedMergeTree counterpart to metrics_mergetree.sql. Requires the
-- GR build/configuration that supplies SharedMergeTree. Not an OSS fallback.
SET enable_time_series_table = 1;
SET default_table_engine = 'SharedMergeTree';

CREATE DATABASE IF NOT EXISTS gr_metrics;

CREATE TABLE IF NOT EXISTS gr_metrics.metrics
ENGINE = TimeSeries
SETTINGS
    samples_index_granularity = 32768,
    recent_samples_index_granularity = 8192,
    recent_samples_ttl_seconds = 21600
SAMPLES INNER COLUMNS
(
    timestamp DateTime64(3) CODEC(Delta, T64, ZSTD(3)),
    value Float64 CODEC(ALP, ZSTD(3))
)
SAMPLES INNER ENGINE = SharedMergeTree
PARTITION BY toDate(timestamp, 'UTC')
ORDER BY (id, timestamp)
TTL toDateTime(timestamp, 'UTC') + INTERVAL 30 DAY DELETE
SETTINGS ttl_only_drop_parts = 1;
