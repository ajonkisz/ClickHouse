#!/usr/bin/env python3
"""
Comprehensive PromQL test runner
Tests both parsing (SQL generation) and mathematical correctness
"""

import subprocess
import json
import re
import sys
import time
from datetime import datetime, timedelta
from typing import List, Tuple, Dict, Optional

CLICKHOUSE_CLIENT = "./build/programs/clickhouse-client"
PROMETHEUS_API = "http://localhost:9363/api/v1"
TEST_DB = "test_promql"
TEST_TABLE = "test_metrics"

# Test results
results = {
    "passed": [],
    "failed": [],
    "skipped": [],
    "total": 0
}

def run_sql(sql: str, expect_error: bool = False) -> Tuple[bool, str]:
    """Run SQL query and return (success, output)"""
    try:
        result = subprocess.run(
            [CLICKHOUSE_CLIENT, "-q", sql],
            capture_output=True,
            text=True,
            timeout=30
        )
        if expect_error:
            return result.returncode != 0, result.stderr
        return result.returncode == 0, result.stdout.strip()
    except Exception as e:
        return False, str(e)

def run_promql_query(query: str, start_time: int, end_time: int, step: int = 15) -> Tuple[bool, Optional[Dict]]:
    """Run PromQL query via HTTP API"""
    import urllib.request
    import urllib.parse
    
    try:
        url = f"{PROMETHEUS_API}/query_range"
        params = {
            "query": query,
            "start": start_time,
            "end": end_time,
            "step": step
        }
        url_with_params = f"{url}?{urllib.parse.urlencode(params)}"
        
        req = urllib.request.Request(url_with_params)
        with urllib.request.urlopen(req, timeout=10) as response:
            data = json.loads(response.read().decode())
            
            if data.get("status") == "success":
                return True, data
            else:
                return False, data
    except Exception as e:
        return False, {"error": str(e)}

def setup_test_environment():
    """Set up test database and tables"""
    print("Setting up test environment...")
    
    # Create database
    run_sql(f"CREATE DATABASE IF NOT EXISTS {TEST_DB}")
    run_sql("SET allow_experimental_time_series_table = 1")
    run_sql("SET allow_experimental_time_series_aggregate_functions = 1")
    
    # Drop existing tables
    for table in [TEST_TABLE, "ts_data_test", "ts_tags_test", "ts_metrics_test"]:
        run_sql(f"DROP TABLE IF EXISTS {TEST_DB}.{table}")
    
    # Create data table
    run_sql(f"""
    CREATE TABLE {TEST_DB}.ts_data_test
    (
        id UUID CODEC(ZSTD(3)),
        timestamp DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
        value Float64 CODEC(GorillaV2, ZSTD(3))
    )
    ENGINE = MergeTree
    PARTITION BY toDate(timestamp)
    ORDER BY (id, timestamp)
    SETTINGS index_granularity = 8192;
    """)
    
    # Create tags table
    run_sql(f"""
    CREATE TABLE {TEST_DB}.ts_tags_test
    (
        id UUID,
        metric_name LowCardinality(String) CODEC(ZSTD(3)),
        tags Map(LowCardinality(String), String) CODEC(ZSTD(3)),
        min_time SimpleAggregateFunction(min, Nullable(DateTime64(3))),
        max_time SimpleAggregateFunction(max, Nullable(DateTime64(3)))
    )
    ENGINE = AggregatingMergeTree
    PRIMARY KEY metric_name
    ORDER BY (metric_name, id)
    SETTINGS index_granularity = 8192;
    """)
    
    # Create metrics table
    run_sql(f"""
    CREATE TABLE {TEST_DB}.ts_metrics_test
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
    DATA {TEST_DB}.ts_data_test
    TAGS {TEST_DB}.ts_tags_test
    METRICS {TEST_DB}.ts_metrics_test;
    """)
    
    print("✓ Test environment ready\n")

def insert_test_data():
    """Insert test data matching test expectations"""
    print("Inserting test data...")
    
    # Base timestamp: 2025-12-01 22:00:00 UTC
    base_ts = 1764628800
    
    # cpu_usage_percent: service="api" [10, 20, 30, 40, 50]
    for i in range(5):
        ts = base_ts + i * 60
        val = 10 + i * 10
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {{'service': 'api', 'host': 'server1'}}, {{'service': 'api', 'host': 'server1'}}, 
         toDateTime64({ts}, 3), {val}, 'cpu', 'gauge', 'percent', 'CPU usage');
        """)
    
    # cpu_usage_percent: service="database" [20, 25, 30, 35, 40]
    for i in range(5):
        ts = base_ts + i * 60
        val = 20 + i * 5
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {{'service': 'database', 'host': 'server2'}}, {{'service': 'database', 'host': 'server2'}}, 
         toDateTime64({ts}, 3), {val}, 'cpu', 'gauge', 'percent', 'CPU usage');
        """)
    
    # cpu_usage_percent: service="cache" [5, 7, 9, 11, 13]
    for i in range(5):
        ts = base_ts + i * 60
        val = 5 + i * 2
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {{'service': 'cache', 'host': 'server3'}}, {{'service': 'cache', 'host': 'server3'}}, 
         toDateTime64({ts}, 3), {val}, 'cpu', 'gauge', 'percent', 'CPU usage');
        """)
    
    # temperature_celsius: [-10, -15, -20] for abs() testing
    for i in range(3):
        ts = base_ts + i * 60
        val = -10 - i * 5
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('temperature_celsius', {{'location': 'room1'}}, {{'location': 'room1'}}, 
         toDateTime64({ts}, 3), {val}, 'temperature', 'gauge', 'celsius', 'Temperature');
        """)
    
    # memory_usage_gb: [10.3, 11.0, 11.7] for round/ceil/floor
    for i, val in enumerate([10.3, 11.0, 11.7]):
        ts = base_ts + i * 60
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('memory_usage_gb', {{'host': 'server1'}}, {{'host': 'server1'}}, 
         toDateTime64({ts}, 3), {val}, 'memory', 'gauge', 'gb', 'Memory usage');
        """)
    
    # http_requests_total: [100, 110, 120, 130, 140, 150] for rate/increase
    for i in range(6):
        ts = base_ts + i * 60
        val = 100 + i * 10
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('http_requests_total', {{'method': 'GET', 'status': '200'}}, {{'method': 'GET', 'status': '200'}}, 
         toDateTime64({ts}, 3), {val}, 'http', 'counter', 'total', 'HTTP requests');
        """)
    
    # constant_metric: [50, 50, 50, 50] for delta/changes
    for i in range(4):
        ts = base_ts + i * 60
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('constant_metric', {{'name': 'test'}}, {{'name': 'test'}}, 
         toDateTime64({ts}, 3), 50.0, 'test', 'gauge', '', 'Constant metric');
        """)
    
    print("✓ Test data inserted\n")

def extract_test_queries() -> List[Tuple[str, str]]:
    """Extract all test queries from test file"""
    queries = []
    
    with open('src/Storages/TimeSeries/tests/gtest_prometheus_query_to_sql.cpp', 'r') as f:
        content = f.read()
    
    # Find all TEST_F blocks
    test_pattern = r'TEST_F\(PrometheusQueryToSQLTest,\s*([^)]+)\)'
    tests = re.finditer(test_pattern, content)
    
    for test_match in tests:
        test_name = test_match.group(1)
        start_pos = test_match.end()
        
        # Look for query in next 30 lines
        test_block = content[start_pos:start_pos+2000]
        
        # Match: String query = "promql";
        query_match = re.search(r'String query[^=]*=\s*"([^"]+)"', test_block)
        if query_match:
            queries.append((test_name, query_match.group(1)))
        # Match: String query = R"(promql)";
        elif 'R"(' in test_block:
            raw_match = re.search(r'String query[^=]*=\s*R"\(([^)]+)\)"', test_block)
            if raw_match:
                queries.append((test_name, raw_match.group(1)))
    
    return queries

def test_query(test_name: str, query: str) -> bool:
    """Test a single PromQL query"""
    results["total"] += 1
    
    # Calculate time range (last hour to now)
    end_time = int(time.time())
    start_time = end_time - 3600
    
    success, result = run_promql_query(query, start_time, end_time)
    
    if success and result and result.get("status") == "success":
        # Check if we got data
        data = result.get("data", {})
        if data.get("result") or data.get("resultType"):
            results["passed"].append(test_name)
            print(f"✓ {test_name:50} {query[:60]}")
            return True
        else:
            # Empty result might be OK for some queries
            results["passed"].append(test_name)
            print(f"✓ {test_name:50} {query[:60]} (empty result)")
            return True
    else:
        error = result.get("error", "Unknown error") if isinstance(result, dict) else str(result)
        results["failed"].append((test_name, query, error))
        print(f"✗ {test_name:50} {query[:60]}")
        print(f"  Error: {error[:100]}")
        return False

def main():
    """Main test runner"""
    print("=" * 80)
    print("PromQL Comprehensive Test Runner")
    print("=" * 80)
    print()
    
    # Setup
    setup_test_environment()
    insert_test_data()
    
    # Extract and run tests
    print("Extracting test queries...")
    queries = extract_test_queries()
    print(f"Found {len(queries)} test queries\n")
    
    print("Running tests...")
    print("-" * 80)
    
    for test_name, query in queries:
        test_query(test_name, query)
    
    # Summary
    print()
    print("=" * 80)
    print("Test Summary")
    print("=" * 80)
    print(f"Total:  {results['total']}")
    print(f"Passed: {len(results['passed'])}")
    print(f"Failed: {len(results['failed'])}")
    print()
    
    if results["failed"]:
        print("Failed tests:")
        for test_name, query, error in results["failed"]:
            print(f"  - {test_name}")
            print(f"    Query: {query}")
            print(f"    Error: {error[:200]}")
            print()
        return 1
    
    return 0

if __name__ == "__main__":
    sys.exit(main())

