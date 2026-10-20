# GR metrics qualification tests

Use stock current master as the baseline. Custom GorillaV2 is unnecessary: its compressor is the existing Gorilla algorithm. Keep DoubleDeltaVarInt as an experimental comparison; do not require it for the first on-prem replay.

## Simple checks already executed

The checks below ran on isolated stock head 26.11.1.290, commit `607e4c1e3f6d8a02669579dc398f2c0c698d3d40`, with MergeTree rather than GR's private SharedMergeTree build.

| Check | Result and scope |
| --- | --- |
| Complete stock codec chains | 240 Wide MergeTree configurations, 262,144 rows each; exact expected keys, timestamps and value bits passed. Compared physical compressed column bytes separately from aggregate estimates. |
| Custom timestamp control | 40 fully framed timestamp-chain cases at equal ZSTD levels 1/3; original VarInt core and all T64 outputs roundtripped. This is a standalone size experiment, not a native custom-codec integration or CPU benchmark. |
| Native Prometheus HTTP API | 12/12 checks: Snappy/protobuf remote write, instant GET, aggregation POST, rate, range query, series/label discovery, exact-batch replay, out-of-order samples, dotted metric names and malformed-query errors. |
| OTel bridge | 9/9 checks through an actual stock Collector: OTLP JSON -> Prometheus remote write -> stock ClickHouse. Cumulative counter, gauge, classic histogram, rate/quantile, normalized labels and timestamps passed. No Kafka was involved. |
| OTel inputs needing conversion | In the plain bridge, OTLP requests containing a delta monotonic sum or cumulative exponential histogram returned HTTP 200, but neither produced a backend series; a cumulative-counter control did. An accepted OTLP request is not proof that every metric was exported. |
| Opt-in stock OTel conversions | 9/9 additional checks passed with `deltatocumulative` and explicit-histogram conversion: deltas 5/7/11 became cumulative 5/12/23; the positive aligned histogram fixture retained count 18, sum 36 and expected buckets/quantiles. No native exponential-histogram preservation or recovery behavior was established. |

The cached Collector image was tagged `0.146.0` but self-reported `0.145.0`; its digest and the discrepancy were recorded. Choose and verify the intended Collector build before production qualification.

The [tested exporter version's documentation](https://github.com/open-telemetry/opentelemetry-collector-contrib/blob/v0.146.0/exporter/prometheusremotewriteexporter/README.md) documents dropping noncumulative monotonic inputs and incomplete remote-write-v2/native-histogram support. The observed omissions establish a coverage gap in this plain bridge, not a demonstrated ClickHouse bug. Delta accumulation and histogram conversion require explicit policy and validation; preserving native exponential histograms is a separate requirement.

The codec tests show no universal winner. Delta/T64 won jittered timestamps; DoubleDelta won perfectly regular intervals. ZSTD(3) did not consistently beat ZSTD(1). T64 after the packed VarInt stream did not improve the tested cases. These are synthetic results, not GR capacity estimates.

## Optional Collector conversions

The following processor fragment was exercised with the test Collector. Add it to an existing metrics pipeline before `batch` and the remote-write exporter only if these conversions meet the workload's requirements:

```yaml
processors:
  deltatocumulative:
    max_stale: 5m
    max_streams: 100 # Smoke limit only; excess streams are dropped.
  transform/explicit_histogram:
    error_mode: propagate
    metric_statements:
      - context: metric
        statements:
          - 'convert_exponential_histogram_to_histogram("upper", [0.0, 1.0, 2.0, 4.0, 8.0])'
```

The tested processor order was `[deltatocumulative, transform/explicit_histogram, batch]`. This is a configuration fragment, not a complete Collector deployment.

The [delta processor](https://github.com/open-telemetry/opentelemetry-collector-contrib/blob/v0.146.0/processor/deltatocumulativeprocessor/README.md) is alpha and accumulates in memory. Size its stream limit, route each series consistently, and test ordering, stale expiry, restarts, rebalances and replay. The smoke does not establish persistent totals across failures.

The [histogram transform](https://github.com/open-telemetry/opentelemetry-collector-contrib/blob/v0.146.0/processor/transformprocessor/README.md#convert_exponential_histogram_to_histogram) is a lossy conversion with explicit target boundaries. The above boundaries suit only the small positive fixture; they are not a GR bucket policy. Test negative values, zeros, misaligned buckets and the resulting quantile error. If preserving native exponential histograms is required, this workaround does not satisfy that requirement.

## Re-run the native API smoke

Use a disposable instance with `metrics_mergetree.sql`, `prometheus_api.xml`, and `enable_time_series_table=1` in the API user's profile. The user needs permission to create the fixture database/table, write samples and query them. Set the password through the named environment variable, then run:

```sh
python3 run/gr/smoke_ingestion.py \
  --base-url http://127.0.0.1:8123 \
  --username gr_test \
  --password-env GR_CH_PASSWORD \
  --schema run/gr/metrics_mergetree.sql \
  --output /tmp/gr-ingestion-smoke.json
```

The script creates the fixture if absent and writes small synthetic metric series with a unique `run` label. Schema statements share a unique HTTP session, and evidence checks the selected default engine and every generated target's engine family. It deletes nothing. `IF NOT EXISTS` does not upgrade an existing schema. Its replay assertion establishes observed query-result deduplication, not physical exactly-once storage or durability through a crash.

## Concrete next tests

| Order | Test | Required evidence / completion criterion |
| --- | --- | --- |
| 1 | Fixed-window GR Kafka replay into a separate consumer group and new metrics table | Save source offsets/builds/conversion policy. Reconcile expected series, labels, timestamps and values after OTel-to-Prometheus expansion. No unexplained dropped/extra points; sample counts alone are insufficient. |
| 2 | Schema A/B using that same replay | Compare timestamp DoubleDelta/ZSTD1 and Delta/T64/ZSTD1/3; value ALP/ZSTD3, Gorilla/ZSTD3 and plain ZSTD; main granularity 8K/32K/64K. Keep all other settings fixed. Record every backing table's disk bytes, marks/indexes, merge CPU, ingest CPU, lag and query latency. |
| 3 | Recent-sample copy enabled versus disabled | Use separately created tables. Compare storage/write amplification and short-query latency. Query ranges wholly inside, outside and crossing the recent window; results must agree. |
| 4 | Consumer restart, rebalance, retry and replay overlap | Reconcile point sets and series identity before/after. Confirm offset acknowledgement and durable ClickHouse acknowledgement behavior. Do not put consumer/collector instance IDs into application metric labels. |
| 5 | OTel semantic fixtures | Counter resets, delta-to-cumulative conversion, missing/stale/NaN values, histogram boundaries, exponential histograms, units/name normalization, resource/scope collisions and sub-millisecond requirements. Required conversions must be explicit; no silent type loss. |
| 6 | Conflicting samples and late data | Distinguish identical retries from different values at the same timestamp. Document the accepted conflict policy and late-data horizon; test behavior before/after merges. |
| 7 | Real dashboards and rules while ingesting | Replay all critical expressions at fixed evaluation times; compare labels/timestamps/values to the current backend. Measure p50/p95/p99 under actual concurrency, cold/warm reads and rule evaluation; agree freshness and latency budgets before acceptance. |
| 8 | Short-lived job/series churn over retention | Record tags/time-ranges/sample growth, cache memory, part counts and query reads as series start/end. Sample TTL alone does not remove tags; establish bounded, affordable metadata behavior. |
| 9 | Private on-prem SharedMergeTree and tenant isolation | Confirm every target uses the intended Shared family and DDL reaches all nodes. Test restart/failover, rolling upgrade, backup/restore and object-storage interruption. Verify SQL, PromQL and metadata endpoints cannot cross tenant policies. |

Start from the checked-in schema as an explicit reference configuration. Do not change the production codec or default solely because a synthetic case won; apply the GR replay and CPU/query results first.

## Upstream contribution decisions

- External LowCardinality metadata coverage follows the useful remaining request from the old storage PR.
- The codec-chain performance test is useful independently of any new codec: it includes equal-level controls and both regular/jittered inputs.
- A new timestamp codec needs a material total-storage/CPU advantage on the representative replay. If such a gap remains, follow the maintainer-requested group-VarInt/frame-of-reference direction, with functional/corruption/boundary tests and an allocated format ID.
- Raise engine fixes from minimized failing fixtures found by the tests above. No new engine feature was necessary for the completed cumulative-counter/gauge/classic-histogram smoke route.
- GR-specific schema and endpoint fixtures belong on this fork; Collector conversion/configuration changes belong in the appropriate Collector/project repository. Native histogram or other unsupported-type work is a separate feature decision if GR needs it.
