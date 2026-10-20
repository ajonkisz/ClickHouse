# GR metrics ingestion baseline

These profiles start a new metrics qualification against current upstream master. They do not claim to be optimal for GR data. The comparison point is AJ's [original 2025 TimeSeries schema](https://github.com/ajonkisz/ClickHouse/blob/promql/run/config/create_ts_inner_tables_config_final.sql), which remains preserved on `promql`.

Use `metrics_mergetree.sql` locally and `metrics_sharedmergetree.sql` on the on-prem build that supplies SharedMergeTree. Both create `gr_metrics.metrics`; use separate services/databases when comparing them. `IF NOT EXISTS` does not update an older schema. Inspect `SHOW CREATE TABLE gr_metrics.metrics` before testing an existing deployment.

```sh
clickhouse-client --multiquery < run/gr/metrics_mergetree.sql
```

The SharedMergeTree profile sets `default_table_engine` so the generated tags, time-ranges, metric-family and recent-sample tables use the corresponding Shared engine family too. The local profile is the executable smoke-test counterpart; the Shared profile requires validation on GR's private build. Apply its DDL through the deployment's catalog/cluster mechanism: selecting SharedMergeTree does not itself distribute these CREATE statements to every query/ingestion node.

The initial choices are explicit:

| Choice | Reason |
| --- | --- |
| `Delta, T64, ZSTD(3)` timestamps and `ALP, ZSTD(3)` values | Match the current master TimeSeries defaults; benchmark other chains before selecting a winner. |
| 32K main / 8K recent granularity | Start from current master defaults, then compare 8K/32K/64K using `tests/performance/codecs_time_series_chains.xml`. |
| Six-hour recent-sample copy | A qualification parameter, not a measured optimum; price the duplicate writes/storage and test ranges crossing the boundary. Set zero in a separate new table to measure its cost. |
| Thirty-day main TTL, UTC day partitions | Bound the local experiment and allow part expiration. These are test retention choices, not GR's final retention policy. |
| Millisecond samples | Match the Prometheus path and the original TimeSeries experiment. This is distinct from the nanosecond raw OTel schemas. |

Install `prometheus_api.xml` under the test server's `config.d` and enable `enable_time_series_table` in the API user's profile. Authenticate through the configured HTTP user/TLS policy. The handler fixes the database and table rather than letting a request select another target.

Configure the Kafka consumer's Prometheus remote-write exporter to use `/prometheus/api/v1/write` on that server. Point Grafana's Prometheus datasource at `/prometheus` (Grafana appends `/api/v1/...`). Test the native label/series/metadata APIs before removing the old Python proxy.

The existing standard OTel ClickHouse exporter writes wide OTel tables, not this TimeSeries schema. Kafka OTLP input therefore still needs a deliberate conversion route: Collector Kafka receiver -> required temporality/name/label conversion -> Prometheus remote-write exporter. Do not inject the consumer instance ID into application metric labels. Preserve the original raw OTel route separately if resource/scope metadata, nanosecond samples or exemplars must remain available for SQL analysis.

Remaining qualification work includes sample replay/deduplication, histogram and temporality semantics, series-metadata growth after TTL expiration, and full dashboard/rule equivalence. A sample TTL does not automatically clean up the tags table. The XML/schema fixtures do not implement those features.

Do not open physical parts written with the old custom codecs under stock upstream. Their method bytes now collide with other codecs (`DoubleDeltaVarInt` with ALP; `GorillaV2` with SZ3). Use a decoder from the old build and a logical export/reingestion plan if such parts need migrating.
