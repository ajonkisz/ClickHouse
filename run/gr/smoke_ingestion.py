"""Bounded, isolated-server Prometheus API smoke test; no external packages."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import struct
import time
import urllib.error
import urllib.parse
import urllib.request


def varint(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128)
        n >>= 7
    out.append(n)
    return bytes(out)


def blob(field, value):
    return varint(field * 8 + 2) + varint(len(value)) + value


def snappy_literal(data):
    n = len(data) - 1
    if n < 60:
        tag = bytes([n << 2])
    else:
        width = max(1, (n.bit_length() + 7) // 8)
        tag = bytes([(59 + width) << 2]) + n.to_bytes(width, "little")
    return varint(len(data)) + tag + data


def write_request(labels, samples):
    series = b"".join(
        blob(1, blob(1, k.encode()) + blob(2, v.encode()))
        for k, v in sorted(labels.items())
    )
    for timestamp, value in samples:
        series += blob(2, b"\x09" + struct.pack("<d", value) + b"\x10" + varint(timestamp))
    return snappy_literal(blob(1, series))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password-env", default="GR_CH_PASSWORD")
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    base = args.base_url.rstrip("/")
    auth = "Basic " + base64.b64encode(
        (args.username + ":" + os.environ[args.password_env]).encode()
    ).decode()
    schema_session = "gr-smoke-schema-" + str(time.time_ns())

    def request(path, params=None, data=None, headers=None):
        if params:
            path += "?" + urllib.parse.urlencode(params)
        req = urllib.request.Request(base + path, data=data, headers={"Authorization": auth, **(headers or {})})
        try:
            with urllib.request.urlopen(req, timeout=30) as response:
                raw = response.read()
                if not raw:
                    result = None
                else:
                    try:
                        result = json.loads(raw)
                    except json.JSONDecodeError:
                        result = [json.loads(line) for line in raw.splitlines() if line]
                return response.status, result
        except urllib.error.HTTPError as error:
            raw = error.read().decode()
            try:
                raw = json.loads(raw)
            except json.JSONDecodeError:
                pass
            return error.code, raw

    def sql(query):
        status, result = request("/", {"session_id": schema_session}, data=(query + " FORMAT JSONEachRow").encode())
        assert status == 200, (status, result)
        return result

    # A session preserves SET values across the separate schema requests.
    schema = args.schema.read_text()
    engine_match = re.search(r"SET\s+default_table_engine\s*=\s*'([A-Za-z]+)'", schema)
    assert engine_match, "The schema must set its intended default_table_engine"
    expected_engine = engine_match.group(1)
    assert expected_engine in {"MergeTree", "SharedMergeTree"}, expected_engine
    for statement in schema.split(";"):
        statement = statement.strip()
        if statement:
            status, result = request("/", {"session_id": schema_session}, data=statement.encode())
            assert status == 200, (status, result)

    settings = sql("SELECT getSetting('default_table_engine') AS default_table_engine")
    assert settings["default_table_engine"] == expected_engine, settings
    targets = sql(
        "SELECT name, engine FROM system.tables WHERE database = 'gr_metrics' "
        "AND startsWith(name, '.inner_id.') AND endsWith(name, "
        "(SELECT toString(uuid) FROM system.tables WHERE database = 'gr_metrics' AND name = 'metrics')) "
        "ORDER BY name"
    )
    if isinstance(targets, dict):
        targets = [targets]
    prefix = "Shared" if expected_engine == "SharedMergeTree" else ""
    allowed_engines = {prefix + name for name in ("MergeTree", "ReplacingMergeTree", "AggregatingMergeTree")}
    assert targets and all(target["engine"] in allowed_engines for target in targets), targets

    run = str(time.time_ns())
    t0 = (int(time.time()) - 60) * 1000
    end = (t0 + 30000) / 1000
    labels = {"__name__": "gr_smoke_counter_total", "job": "gr", "host": "h1", "run": run}
    selector = 'gr_smoke_counter_total{run="' + run + '"}'
    payload = write_request(labels, [(t0, 100), (t0 + 15000, 115), (t0 + 30000, 130)])
    evidence = {"scope": "Isolated HTTP remote-write/query smoke; does not exercise Kafka, Collector, failover or load", "checks": [], "observations": {"schema_settings": settings, "target_engines": targets}}

    def check(name, fn):
        try:
            detail = fn()
            evidence["checks"].append({"name": name, "passed": True, "detail": detail})
        except Exception as error:
            evidence["checks"].append({"name": name, "passed": False, "error": repr(error)})

    def write(data):
        status, result = request(
            "/prometheus/api/v1/write",
            {"async_insert_busy_timeout_max_ms": "20"}, data,
            {"Content-Type": "application/x-protobuf", "Content-Encoding": "snappy", "X-Prometheus-Remote-Write-Version": "0.1.0"},
        )
        assert 200 <= status < 300, (status, result)
        return status

    def query(expression, post=False):
        params = {"query": expression, "time": str(end)}
        if post:
            status, result = request("/prometheus/api/v1/query", data=urllib.parse.urlencode(params).encode(), headers={"Content-Type": "application/x-www-form-urlencoded"})
        else:
            status, result = request("/prometheus/api/v1/query", params)
        assert status == 200 and result["status"] == "success", (status, result)
        return result["data"]["result"]

    def value_check(expression, expected, post=False):
        result = query(expression, post)
        assert len(result) == 1 and abs(float(result[0]["value"][1]) - expected) < 1e-9, result
        return result

    check("remote_write_v1_snappy_protobuf", lambda: write(payload))
    check("instant_get", lambda: value_check(selector, 130))
    check("aggregation_post", lambda: value_check("sum by(job)(" + selector + ")", 130, True))
    check("counter_rate", lambda: value_check("rate(" + selector + "[30s])", 1))

    def range_check():
        status, result = request("/prometheus/api/v1/query_range", {"query": selector, "start": str(t0 / 1000), "end": str(end), "step": "15s"})
        assert status == 200 and result["status"] == "success", result
        points = result["data"]["result"][0]["values"]
        assert [float(p[1]) for p in points] == [100, 115, 130], points
        return points

    check("range_query", range_check)

    def metadata_check(path, expected):
        status, result = request(path, {"match[]": selector, "start": str(t0 / 1000), "end": str(end)})
        assert status == 200 and result["status"] == "success", result
        assert expected in result["data"], result
        return result["data"]

    check("series_discovery", lambda: metadata_check("/prometheus/api/v1/series", labels))
    check("label_names", lambda: metadata_check("/prometheus/api/v1/labels", "host"))
    check("label_values", lambda: metadata_check("/prometheus/api/v1/label/host/values", "h1"))

    def replay_check():
        write(payload)
        value_check("sum by(job)(" + selector + ")", 130)
        result = query(selector + "[2m]")
        assert len(result) == 1 and len(result[0]["values"]) == 3, result
        return result[0]["values"]

    check("exact_batch_replay_query_deduplication", replay_check)

    def out_of_order_check():
        write(write_request(labels, [(t0 + 5000, 105)]))
        result = query(selector + "[2m]")
        points = result[0]["values"]
        assert [float(p[1]) for p in points] == [100, 105, 115, 130], result
        return points

    check("out_of_order_sample", out_of_order_check)

    def dotted_name_check():
        write(write_request({"__name__": "gr_smoke.dot", "run": run}, [(t0 + 30000, 42)]))
        return value_check('{__name__="gr_smoke.dot",run="' + run + '"}', 42)

    check("dotted_metric_name", dotted_name_check)

    def invalid_query_check():
        status, result = request("/prometheus/api/v1/query", {"query": "sum("})
        assert status >= 400 and isinstance(result, dict) and result.get("status") == "error", (status, result)
        return status

    check("invalid_promql_returns_error", invalid_query_check)
    evidence["observations"]["build"] = sql("SELECT version() AS version")
    evidence["passed"] = sum(c["passed"] for c in evidence["checks"])
    evidence["total"] = len(evidence["checks"])
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence, indent=2))
    return 0 if evidence["passed"] == evidence["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
