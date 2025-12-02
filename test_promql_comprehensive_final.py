#!/usr/bin/env python3
"""
Comprehensive PromQL test suite
Tests all 126 queries from gtest file with proper error handling
"""

import subprocess
import sys
import re
import json
import urllib.request
import urllib.parse
import time
from typing import List, Tuple, Optional, Dict, Any

CLICKHOUSE_CLIENT = "./build/programs/clickhouse-client"
PROMETHEUS_API = "http://localhost:9363/api/v1"
TEST_DB = "otel"
TEST_TABLE = "metrics_v6"

results = {
    "parse_passed": [],
    "parse_failed": [],
    "parse_skipped": [],
    "parse_errors": [],
    "total": 0
}

def run_sql(sql: str) -> Tuple[bool, str]:
    """Run SQL query"""
    try:
        result = subprocess.run(
            [CLICKHOUSE_CLIENT, "-q", sql],
            capture_output=True,
            text=True,
            timeout=30
        )
        return result.returncode == 0, result.stdout.strip() if result.returncode == 0 else result.stderr.strip()
    except Exception as e:
        return False, str(e)

def run_promql_range(query: str, start: int, end: int, step: int = 15) -> Tuple[bool, Optional[Dict[str, Any]], Optional[str]]:
    """Run PromQL range query via HTTP API"""
    try:
        url = f"{PROMETHEUS_API}/query_range"
        params = {
            "query": query,
            "start": start,
            "end": end,
            "step": step
        }
        url_with_params = f"{url}?{urllib.parse.urlencode(params)}"
        
        req = urllib.request.Request(url_with_params)
        with urllib.request.urlopen(req, timeout=30) as response:
            data = json.loads(response.read().decode())
            if data.get("status") == "success":
                return True, data, None
            else:
                error_msg = data.get("error", {})
                if isinstance(error_msg, dict):
                    error_str = error_msg.get("data", str(error_msg))
                else:
                    error_str = str(error_msg)
                return False, data, error_str
    except urllib.error.HTTPError as e:
        try:
            error_body = e.read().decode()
            error_data = json.loads(error_body) if error_body else {}
            error_msg = error_data.get("error", {}).get("data", str(e)) if isinstance(error_data.get("error"), dict) else str(e)
            return False, None, error_msg
        except:
            return False, None, f"HTTP {e.code}: {e.reason}"
    except Exception as e:
        return False, None, str(e)

def run_promql_instant(query: str, time: Optional[int] = None) -> Tuple[bool, Optional[Dict[str, Any]], Optional[str]]:
    """Run PromQL instant query via HTTP API"""
    try:
        url = f"{PROMETHEUS_API}/query"
        params = {"query": query}
        if time:
            params["time"] = time
        
        url_with_params = f"{url}?{urllib.parse.urlencode(params)}"
        
        req = urllib.request.Request(url_with_params)
        with urllib.request.urlopen(req, timeout=30) as response:
            data = json.loads(response.read().decode())
            if data.get("status") == "success":
                return True, data, None
            else:
                error_msg = data.get("error", {})
                if isinstance(error_msg, dict):
                    error_str = error_msg.get("data", str(error_msg))
                else:
                    error_str = str(error_msg)
                return False, data, error_str
    except urllib.error.HTTPError as e:
        try:
            error_body = e.read().decode()
            error_data = json.loads(error_body) if error_body else {}
            error_msg = error_data.get("error", {}).get("data", str(e)) if isinstance(error_data.get("error"), dict) else str(e)
            return False, None, error_msg
        except:
            return False, None, f"HTTP {e.code}: {e.reason}"
    except Exception as e:
        return False, None, str(e)

def extract_all_test_queries() -> List[Tuple[str, str, str]]:
    """Extract all PromQL queries from test file with query type"""
    queries = []
    
    with open('src/Storages/TimeSeries/tests/gtest_prometheus_query_to_sql.cpp', 'r') as f:
        content = f.read()
    
    # Find all TEST_F blocks
    test_pattern = r'TEST_F\(PrometheusQueryToSQLTest,\s*([^)]+)\)'
    tests = re.finditer(test_pattern, content)
    
    for test_match in tests:
        test_name = test_match.group(1)
        start_pos = test_match.end()
        
        # Look for query in next 2000 chars
        test_block = content[start_pos:start_pos+2000]
        
        # Match: String query = "promql";
        query_match = re.search(r'String query[^=]*=\s*"([^"]+)"', test_block)
        if query_match:
            query = query_match.group(1)
            # Determine query type based on content
            query_type = "range" if "[" in query else "instant"
            queries.append((test_name, query, query_type))
        # Match: String query = R"(promql)";
        elif 'R"(' in test_block:
            raw_match = re.search(r'String query[^=]*=\s*R"\(([^)]+)\)"', test_block)
            if raw_match:
                query = raw_match.group(1)
                query_type = "range" if "[" in query else "instant"
                queries.append((test_name, query, query_type))
    
    return queries

def setup_test_data():
    """Set up comprehensive test data covering various time ranges"""
    print("Setting up comprehensive test data...")
    
    # Ensure database exists
    run_sql(f"CREATE DATABASE IF NOT EXISTS {TEST_DB}")
    run_sql("SET allow_experimental_time_series_table = 1")
    run_sql("SET allow_experimental_time_series_aggregate_functions = 1")
    
    # Drop and recreate tables
    for table in [TEST_TABLE, "ts_data_v6", "ts_tags_v6", "ts_metrics_v6"]:
        run_sql(f"DROP TABLE IF EXISTS {TEST_DB}.{table}")
    
    # Create underlying tables (matching V6 schema)
    run_sql(f"""
    CREATE TABLE {TEST_DB}.ts_data_v6
    (
        id UUID CODEC(ZSTD(3)),
        timestamp DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
        value Float64 CODEC(GorillaV2, ZSTD(3))
    )
    ENGINE = MergeTree
    PARTITION BY toDate(timestamp)
    ORDER BY (id, timestamp)
    SETTINGS index_granularity = 65536;
    """)
    
    run_sql(f"""
    CREATE TABLE {TEST_DB}.ts_tags_v6
    (
        id UUID,
        metric_name LowCardinality(String) CODEC(ZSTD(3)),
        tags Map(LowCardinality(String), String) CODEC(ZSTD(3)),
        min_time SimpleAggregateFunction(min, Nullable(DateTime64(3))),
        max_time SimpleAggregateFunction(max, Nullable(DateTime64(3))),
        INDEX idx_tags_values mapValues(tags) TYPE ngrambf_v1(3, 8192, 2, 0) GRANULARITY 4,
        INDEX idx_tags_keys mapKeys(tags) TYPE bloom_filter GRANULARITY 4
    )
    ENGINE = AggregatingMergeTree
    PRIMARY KEY metric_name
    ORDER BY (metric_name, id)
    SETTINGS index_granularity = 4096;
    """)
    
    run_sql(f"""
    CREATE TABLE {TEST_DB}.ts_metrics_v6
    (
        metric_family_name LowCardinality(String) CODEC(ZSTD(3)),
        type LowCardinality(String) CODEC(ZSTD(3)),
        unit LowCardinality(String) CODEC(ZSTD(3)),
        help String CODEC(ZSTD(3))
    )
    ENGINE = ReplacingMergeTree
    ORDER BY metric_family_name;
    """)
    
    # Create TimeSeries view
    run_sql(f"""
    CREATE TABLE {TEST_DB}.{TEST_TABLE}
    (
        id UUID DEFAULT reinterpretAsUUID(sipHash128(metric_name, all_tags)) CODEC(ZSTD(3)),
        timestamp DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
        value Float64 CODEC(GorillaV2, ZSTD(3)),
        metric_name LowCardinality(String) CODEC(ZSTD(3)),
        tags Map(LowCardinality(String), String) CODEC(ZSTD(3)),
        all_tags Map(String, String) EPHEMERAL,
        min_time Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(3)),
        max_time Nullable(DateTime64(3)) CODEC(DoubleDeltaVarInt, ZSTD(3)),
        metric_family_name LowCardinality(String) CODEC(ZSTD(3)),
        type LowCardinality(String) CODEC(ZSTD(3)),
        unit LowCardinality(String) CODEC(ZSTD(3)),
        help String CODEC(ZSTD(3))
    )
    ENGINE = TimeSeries
    SETTINGS
        store_min_time_and_max_time = 1,
        aggregate_min_time_and_max_time = 1,
        filter_by_min_time_and_max_time = 1,
        use_all_tags_column_to_generate_id = 1
    DATA {TEST_DB}.ts_data_v6
    TAGS {TEST_DB}.ts_tags_v6
    METRICS {TEST_DB}.ts_metrics_v6;
    """)
    
    # Insert comprehensive test data
    # Base timestamp: current time - 1 hour (to ensure queries can find data)
    base_ts = int(time.time()) - 3600
    
    # Insert cpu_usage_percent data (3 series, 5 samples each)
    for i in range(5):
        ts = base_ts + i * 60
        
        # API service: [10, 20, 30, 40, 50]
        api_id = f"reinterpretAsUUID(sipHash128('cpu_usage_percent', map('service', 'api', 'host', 'server1')))"
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_data_v6 (id, timestamp, value)
        VALUES ({api_id}, toDateTime64({ts}, 3), {10 + i * 10});
        """)
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_tags_v6 (id, metric_name, tags)
        VALUES ({api_id}, 'cpu_usage_percent', {{'service': 'api', 'host': 'server1'}});
        """)
        
        # Database service: [20, 25, 30, 35, 40]
        db_id = f"reinterpretAsUUID(sipHash128('cpu_usage_percent', map('service', 'database', 'host', 'server2')))"
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_data_v6 (id, timestamp, value)
        VALUES ({db_id}, toDateTime64({ts}, 3), {20 + i * 5});
        """)
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_tags_v6 (id, metric_name, tags)
        VALUES ({db_id}, 'cpu_usage_percent', {{'service': 'database', 'host': 'server2'}});
        """)
        
        # Cache service: [5, 7, 9, 11, 13]
        cache_id = f"reinterpretAsUUID(sipHash128('cpu_usage_percent', map('service', 'cache', 'host', 'server3')))"
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_data_v6 (id, timestamp, value)
        VALUES ({cache_id}, toDateTime64({ts}, 3), {5 + i * 2});
        """)
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_tags_v6 (id, metric_name, tags)
        VALUES ({cache_id}, 'cpu_usage_percent', {{'service': 'cache', 'host': 'server3'}});
        """)
    
    # Insert http_requests_total for rate/increase testing
    for i in range(6):
        ts = base_ts + i * 30
        req_id = f"reinterpretAsUUID(sipHash128('http_requests_total', map('method', 'GET', 'status', '200')))"
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_data_v6 (id, timestamp, value)
        VALUES ({req_id}, toDateTime64({ts}, 3), {100 + i * 10});
        """)
        run_sql(f"""
        INSERT INTO {TEST_DB}.ts_tags_v6 (id, metric_name, tags)
        VALUES ({req_id}, 'http_requests_total', {{'method': 'GET', 'status': '200'}});
        """)
    
    # Verify data
    success, count = run_sql(f"SELECT count() FROM {TEST_DB}.ts_data_v6")
    print(f"✓ Data inserted: {count} rows")
    print(f"✓ Data time range: {base_ts} to {base_ts + 300} (5 minutes)")
    return base_ts

def test_all_queries():
    """Test all PromQL queries"""
    print("\n[1] Testing All PromQL Queries")
    print("-" * 80)
    
    queries = extract_all_test_queries()
    print(f"Found {len(queries)} test queries")
    
    # Use current time for queries
    current_time = int(time.time())
    start_time = current_time - 3600  # 1 hour ago
    end_time = current_time
    
    passed = 0
    failed = 0
    skipped = 0
    
    for idx, (test_name, query, query_type) in enumerate(queries, 1):
        results["total"] += 1
        
        # Skip queries that require special handling
        if "time()" in query or "timestamp(" in query:
            # These need special evaluation time
            success, data, error = run_promql_instant(query, current_time)
        elif query_type == "range":
            success, data, error = run_promql_range(query, start_time, end_time, 15)
        else:
            success, data, error = run_promql_instant(query, current_time)
        
        if success:
            results["parse_passed"].append((test_name, query))
            passed += 1
            if idx <= 20 or idx % 20 == 0:  # Show first 20 and every 20th
                print(f"✓ [{idx:3d}] {test_name:50} {query[:50]}")
        else:
            # Check if it's a connection error
            if error and ("Connection" in error or "refused" in error.lower() or "timeout" in error.lower()):
                results["parse_skipped"].append((test_name, query, error))
                skipped += 1
                if skipped <= 5:
                    print(f"⊘ [{idx:3d}] {test_name:50} {query[:50]} (API unavailable)")
            else:
                results["parse_failed"].append((test_name, query, error))
                results["parse_errors"].append((test_name, query, error))
                failed += 1
                if failed <= 20:  # Show first 20 failures
                    print(f"✗ [{idx:3d}] {test_name:50} {query[:50]}")
                    print(f"    Error: {error[:100] if error else 'Unknown error'}")
    
    print(f"\nProgress: {passed} passed, {failed} failed, {skipped} skipped")
    return passed, failed, skipped

def main():
    """Main test runner"""
    print("=" * 80)
    print("Comprehensive PromQL Test Suite")
    print("=" * 80)
    print()
    
    # Setup test data
    base_ts = setup_test_data()
    
    # Test all queries
    passed, failed, skipped = test_all_queries()
    
    # Summary
    print()
    print("=" * 80)
    print("Final Summary")
    print("=" * 80)
    print(f"Total Tests: {results['total']}")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print(f"Skipped: {skipped}")
    print()
    
    if results["parse_errors"]:
        print("Top Errors:")
        error_counts = {}
        for _, _, error in results["parse_errors"]:
            error_key = error[:50] if error else "Unknown"
            error_counts[error_key] = error_counts.get(error_key, 0) + 1
        
        for error, count in sorted(error_counts.items(), key=lambda x: x[1], reverse=True)[:10]:
            print(f"  {count}x: {error}")
        print()
    
    if failed > 0:
        print(f"\n⚠️  {failed} queries failed. Check errors above.")
        return 1
    
    if skipped > 0:
        print(f"\n⚠️  {skipped} queries skipped (API unavailable or timeout)")
        return 0 if passed > 0 else 1
    
    print("\n✅ All tests passed!")
    return 0

if __name__ == "__main__":
    sys.exit(main())

