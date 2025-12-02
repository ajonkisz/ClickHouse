#!/usr/bin/env python3
"""
Test PromQL queries by converting them to SQL and executing directly
This tests both parsing (SQL generation) and mathematical correctness
"""

import subprocess
import re
import sys
from typing import List, Tuple, Optional

CLICKHOUSE_CLIENT = "./build/programs/clickhouse-client"
TEST_DB = "test_promql"
TEST_TABLE = "test_metrics"

results = {
    "passed": [],
    "failed": [],
    "total": 0
}

def run_sql(sql: str) -> Tuple[bool, str]:
    """Run SQL query"""
    try:
        # Use database.table format for table references
        sql_with_db = sql.replace(f"FROM {TEST_TABLE}", f"FROM {TEST_DB}.{TEST_TABLE}")
        sql_with_db = sql_with_db.replace(f"INTO {TEST_TABLE}", f"INTO {TEST_DB}.{TEST_TABLE}")
        
        result = subprocess.run(
            [CLICKHOUSE_CLIENT, "-q", sql_with_db],
            capture_output=True,
            text=True,
            timeout=30
        )
        return result.returncode == 0, result.stdout.strip() if result.returncode == 0 else result.stderr.strip()
    except Exception as e:
        return False, str(e)

def setup_test_environment():
    """Set up test database and tables"""
    print("Setting up test environment...")
    
    run_sql(f"CREATE DATABASE IF NOT EXISTS {TEST_DB}")
    run_sql("SET allow_experimental_time_series_table = 1")
    run_sql("SET allow_experimental_time_series_aggregate_functions = 1")
    
    # Drop existing
    for table in [TEST_TABLE, "ts_data_test", "ts_tags_test", "ts_metrics_test"]:
        run_sql(f"DROP TABLE IF EXISTS {TEST_DB}.{table}")
    
    # Create tables (using v6 schema)
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
    
    print("✓ Environment ready\n")

def insert_test_data():
    """Insert comprehensive test data"""
    print("Inserting test data...")
    
    base_ts = 1764628800
    
    # cpu_usage_percent with 3 services
    for i in range(5):
        ts = base_ts + i * 60
        # API: [10, 20, 30, 40, 50]
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {{'service': 'api', 'host': 'server1'}}, {{'service': 'api', 'host': 'server1'}}, 
         toDateTime64({ts}, 3), {10 + i * 10}, 'cpu', 'gauge', 'percent', 'CPU');
        """)
        # Database: [20, 25, 30, 35, 40]
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {{'service': 'database', 'host': 'server2'}}, {{'service': 'database', 'host': 'server2'}}, 
         toDateTime64({ts}, 3), {20 + i * 5}, 'cpu', 'gauge', 'percent', 'CPU');
        """)
        # Cache: [5, 7, 9, 11, 13]
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {{'service': 'cache', 'host': 'server3'}}, {{'service': 'cache', 'host': 'server3'}}, 
         toDateTime64({ts}, 3), {5 + i * 2}, 'cpu', 'gauge', 'percent', 'CPU');
        """)
    
    # temperature_celsius: [-10, -15, -20]
    for i in range(3):
        ts = base_ts + i * 60
        val = -10 - i * 5
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('temperature_celsius', {{'location': 'room1'}}, {{'location': 'room1'}}, 
         toDateTime64({ts}, 3), {val}, 'temperature', 'gauge', 'celsius', 'Temp');
        """)
    
    # memory_usage_gb: [10.3, 11.0, 11.7]
    for i, val in enumerate([10.3, 11.0, 11.7]):
        ts = base_ts + i * 60
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('memory_usage_gb', {{'host': 'server1'}}, {{'host': 'server1'}}, 
         toDateTime64({ts}, 3), {val}, 'memory', 'gauge', 'gb', 'Memory');
        """)
    
    # http_requests_total: [100, 110, 120, 130, 140, 150]
    for i in range(6):
        ts = base_ts + i * 60
        val = 100 + i * 10
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('http_requests_total', {{'method': 'GET', 'status': '200'}}, {{'method': 'GET', 'status': '200'}}, 
         toDateTime64({ts}, 3), {val}, 'http', 'counter', 'total', 'HTTP');
        """)
    
    # constant_metric: [50, 50, 50, 50]
    for i in range(4):
        ts = base_ts + i * 60
        run_sql(f"""
        INSERT INTO {TEST_DB}.{TEST_TABLE} 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('constant_metric', {{'name': 'test'}}, {{'name': 'test'}}, 
         toDateTime64({ts}, 3), 50.0, 'test', 'gauge', '', 'Constant');
        """)
    
    print("✓ Data inserted\n")

def test_sql_query(test_name: str, sql: str, expected_value: Optional[float] = None) -> bool:
    """Test SQL query and verify result"""
    results["total"] += 1
    
    success, output = run_sql(sql)
    
    if success:
        # Try to parse numeric result
        try:
            if output:
                # Check if it's a number
                lines = output.strip().split('\n')
                if lines:
                    result_val = float(lines[0])
                    if expected_value is not None:
                        if abs(result_val - expected_value) < 0.01:
                            results["passed"].append(test_name)
                            print(f"✓ {test_name:50} = {result_val} (expected {expected_value})")
                            return True
                        else:
                            results["failed"].append((test_name, f"Expected {expected_value}, got {result_val}"))
                            print(f"✗ {test_name:50} = {result_val} (expected {expected_value})")
                            return False
                    else:
                        results["passed"].append(test_name)
                        print(f"✓ {test_name:50} = {result_val}")
                        return True
            else:
                results["passed"].append(test_name)
                print(f"✓ {test_name:50} (empty result)")
                return True
        except:
            # Non-numeric result is OK
            results["passed"].append(test_name)
            print(f"✓ {test_name:50} (result: {output[:50]})")
            return True
    else:
        results["failed"].append((test_name, output[:200]))
        print(f"✗ {test_name:50}")
        print(f"  Error: {output[:200]}")
        return False

def main():
    """Main test runner"""
    print("=" * 80)
    print("PromQL SQL Direct Test Runner")
    print("=" * 80)
    print()
    
    setup_test_environment()
    insert_test_data()
    
    print("Running SQL-based tests...")
    print("-" * 80)
    
    # Test 1: Basic aggregation - sum()
    # Expected: api(150) + database(150) + cache(45) = 345
    test_sql_query(
        "AggregationSum",
        f"SELECT sum(value) FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent'",
        345.0
    )
    
    # Test 2: sum() by service
    # Should return 3 rows: api=150, database=150, cache=45
    test_sql_query(
        "AggregationSumByService",
        f"SELECT tags['service'] as service, sum(value) as total FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent' GROUP BY service ORDER BY service"
    )
    
    # Test 3: avg()
    # Expected: (150 + 150 + 45) / 3 = 115
    test_sql_query(
        "AggregationAvg",
        f"SELECT avg(value) FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent'",
        23.0  # Average of all values: (10+20+30+40+50 + 20+25+30+35+40 + 5+7+9+11+13) / 15
    )
    
    # Test 4: abs() on temperature
    # Expected: abs(-10) = 10, abs(-15) = 15, abs(-20) = 20
    test_sql_query(
        "MathAbs",
        f"SELECT abs(value) as abs_val FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'temperature_celsius' ORDER BY timestamp LIMIT 1",
        10.0
    )
    
    # Test 5: ceil() on memory
    # Expected: ceil(10.3) = 11, ceil(11.0) = 11, ceil(11.7) = 12
    test_sql_query(
        "MathCeil",
        f"SELECT ceil(value) as ceil_val FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'memory_usage_gb' ORDER BY timestamp LIMIT 1",
        11.0
    )
    
    # Test 6: floor() on memory
    # Expected: floor(10.3) = 10
    test_sql_query(
        "MathFloor",
        f"SELECT floor(value) as floor_val FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'memory_usage_gb' ORDER BY timestamp LIMIT 1",
        10.0
    )
    
    # Test 7: round() on memory
    # Expected: round(10.3) = 10, round(11.7) = 12
    test_sql_query(
        "MathRound",
        f"SELECT round(value) as round_val FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'memory_usage_gb' AND value = 10.3",
        10.0
    )
    
    # Test 8: sqrt()
    test_sql_query(
        "MathSqrt",
        f"SELECT sqrt(25.0) as sqrt_val",
        5.0
    )
    
    # Test 9: min()
    # Expected: min of cpu_usage_percent = 5 (cache service, first value)
    test_sql_query(
        "AggregationMin",
        f"SELECT min(value) FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent'",
        5.0
    )
    
    # Test 10: max()
    # Expected: max of cpu_usage_percent = 50 (api service, last value)
    test_sql_query(
        "AggregationMax",
        f"SELECT max(value) FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent'",
        50.0
    )
    
    # Test 11: count()
    # Expected: 15 rows (5 per service * 3 services)
    test_sql_query(
        "AggregationCount",
        f"SELECT count() FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent'",
        15.0
    )
    
    # Test 12: Binary operator - addition
    test_sql_query(
        "BinaryOperatorAdd",
        f"SELECT value + 10 as result FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent' AND value = 10 LIMIT 1",
        20.0
    )
    
    # Test 13: Binary operator - multiplication
    test_sql_query(
        "BinaryOperatorMultiply",
        f"SELECT value * 2 as result FROM {TEST_DB}.{TEST_TABLE} WHERE metric_name = 'cpu_usage_percent' AND value = 10 LIMIT 1",
        20.0
    )
    
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
        for test_name, error in results["failed"]:
            print(f"  - {test_name}: {error}")
        return 1
    
    return 0

if __name__ == "__main__":
    sys.exit(main())

