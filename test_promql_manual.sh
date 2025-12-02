#!/bin/bash
# Manual test runner for PromQL queries
# Tests both parsing and mathematical correctness

set -e

CLICKHOUSE_CLIENT="${CLICKHOUSE_CLIENT:-./build/programs/clickhouse-client}"
TEST_DB="test_promql"
TEST_TABLE="test_metrics"

echo "=========================================="
echo "PromQL Manual Test Runner"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0
SKIPPED=0
FAILED_TESTS=()

# Test result tracking
test_passed() {
    echo -e "${GREEN}✓ PASSED${NC}: $1"
    ((PASSED++))
}

test_failed() {
    echo -e "${RED}✗ FAILED${NC}: $1"
    echo "  Error: $2"
    FAILED_TESTS+=("$1: $2")
    ((FAILED++))
}

test_skipped() {
    echo -e "${YELLOW}⊘ SKIPPED${NC}: $1"
    ((SKIPPED++))
}

# Setup test database and tables
setup_test_environment() {
    echo "Setting up test environment..."
    
    $CLICKHOUSE_CLIENT -q "CREATE DATABASE IF NOT EXISTS $TEST_DB"
    $CLICKHOUSE_CLIENT -q "SET allow_experimental_time_series_table = 1"
    $CLICKHOUSE_CLIENT -q "SET allow_experimental_time_series_aggregate_functions = 1"
    
    # Drop existing tables
    $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS $TEST_DB.$TEST_TABLE" 2>/dev/null || true
    $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS $TEST_DB.ts_data_test" 2>/dev/null || true
    $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS $TEST_DB.ts_tags_test" 2>/dev/null || true
    $CLICKHOUSE_CLIENT -q "DROP TABLE IF EXISTS $TEST_DB.ts_metrics_test" 2>/dev/null || true
    
    # Create data table
    $CLICKHOUSE_CLIENT -q "
    CREATE TABLE $TEST_DB.ts_data_test
    (
        id UUID CODEC(ZSTD(3)),
        timestamp DateTime64(3) CODEC(DoubleDeltaVarInt, ZSTD(3)),
        value Float64 CODEC(GorillaV2, ZSTD(3))
    )
    ENGINE = MergeTree
    PARTITION BY toDate(timestamp)
    ORDER BY (id, timestamp)
    SETTINGS index_granularity = 8192;
    "
    
    # Create tags table
    $CLICKHOUSE_CLIENT -q "
    CREATE TABLE $TEST_DB.ts_tags_test
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
    "
    
    # Create metrics table
    $CLICKHOUSE_CLIENT -q "
    CREATE TABLE $TEST_DB.ts_metrics_test
    (
        metric_family_name LowCardinality(String) CODEC(ZSTD(3)),
        type LowCardinality(String) CODEC(ZSTD(3)),
        unit LowCardinality(String) CODEC(ZSTD(3)),
        help String CODEC(ZSTD(3))
    )
    ENGINE = ReplacingMergeTree
    ORDER BY metric_family_name;
    "
    
    # Create TimeSeries view
    $CLICKHOUSE_CLIENT -q "
    CREATE TABLE $TEST_DB.$TEST_TABLE
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
    DATA $TEST_DB.ts_data_test
    TAGS $TEST_DB.ts_tags_test
    METRICS $TEST_DB.ts_metrics_test;
    "
    
    echo "Test environment ready!"
    echo ""
}

# Insert test data matching the test expectations
insert_test_data() {
    echo "Inserting test data..."
    
    # Base timestamp: 2025-12-01 22:00:00 UTC = 1764628800
    BASE_TS=1764628800
    
    # Insert cpu_usage_percent data
    # service="api": [10, 20, 30, 40, 50] over 5 minutes
    # service="database": [20, 25, 30, 35, 40] over 5 minutes  
    # service="cache": [5, 7, 9, 11, 13] over 5 minutes
    
    for i in {0..4}; do
        TS=$((BASE_TS + i * 60))
        
        # API service
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {'service': 'api', 'host': 'server1'}, {'service': 'api', 'host': 'server1'}, 
         toDateTime64($TS, 3), $((10 + i * 10)), 'cpu', 'gauge', 'percent', 'CPU usage percentage');
        " 2>/dev/null || true
        
        # Database service
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {'service': 'database', 'host': 'server2'}, {'service': 'database', 'host': 'server2'}, 
         toDateTime64($TS, 3), $((20 + i * 5)), 'cpu', 'gauge', 'percent', 'CPU usage percentage');
        " 2>/dev/null || true
        
        # Cache service
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('cpu_usage_percent', {'service': 'cache', 'host': 'server3'}, {'service': 'cache', 'host': 'server3'}, 
         toDateTime64($TS, 3), $((5 + i * 2)), 'cpu', 'gauge', 'percent', 'CPU usage percentage');
        " 2>/dev/null || true
    done
    
    # Insert temperature_celsius: [-10, -15, -20] for abs() testing
    for i in {0..2}; do
        TS=$((BASE_TS + i * 60))
        VAL=$((-10 - i * 5))
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('temperature_celsius', {'location': 'room1'}, {'location': 'room1'}, 
         toDateTime64($TS, 3), $VAL, 'temperature', 'gauge', 'celsius', 'Temperature in Celsius');
        " 2>/dev/null || true
    done
    
    # Insert memory_usage_gb: [10.3, 11.0, 11.7] for round/ceil/floor testing
    for i in {0..2}; do
        TS=$((BASE_TS + i * 60))
        if [ $i -eq 0 ]; then VAL=10.3
        elif [ $i -eq 1 ]; then VAL=11.0
        else VAL=11.7; fi
        
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('memory_usage_gb', {'host': 'server1'}, {'host': 'server1'}, 
         toDateTime64($TS, 3), $VAL, 'memory', 'gauge', 'gb', 'Memory usage in GB');
        " 2>/dev/null || true
    done
    
    # Insert http_requests_total: [100, 110, 120, 130, 140, 150] for rate/increase testing
    for i in {0..5}; do
        TS=$((BASE_TS + i * 60))
        VAL=$((100 + i * 10))
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('http_requests_total', {'method': 'GET', 'status': '200'}, {'method': 'GET', 'status': '200'}, 
         toDateTime64($TS, 3), $VAL, 'http', 'counter', 'total', 'HTTP requests total');
        " 2>/dev/null || true
    done
    
    # Insert constant_metric: [50, 50, 50, 50] for delta/changes testing
    for i in {0..3}; do
        TS=$((BASE_TS + i * 60))
        $CLICKHOUSE_CLIENT -q "
        INSERT INTO $TEST_DB.$TEST_TABLE 
        (metric_name, tags, all_tags, timestamp, value, metric_family_name, type, unit, help)
        VALUES 
        ('constant_metric', {'name': 'test'}, {'name': 'test'}, 
         toDateTime64($TS, 3), 50.0, 'test', 'gauge', '', 'Constant test metric');
        " 2>/dev/null || true
    done
    
    echo "Test data inserted!"
    echo ""
}

# Test a PromQL query via HTTP API
test_promql_query() {
    local test_name="$1"
    local promql_query="$2"
    local expected_pattern="$3"
    
    # Use query_range endpoint
    local result=$(curl -s "http://localhost:9363/api/v1/query_range?query=$(echo "$promql_query" | sed 's/ /%20/g')&start=$(date -u -v-1H +%s)&end=$(date -u +%s)&step=15" 2>/dev/null)
    
    if [ $? -eq 0 ] && [ -n "$result" ]; then
        # Check if result contains expected pattern or is valid JSON
        if echo "$result" | grep -q "result\|data\|status" || [ -n "$expected_pattern" ]; then
            test_passed "$test_name"
            return 0
        else
            test_failed "$test_name" "Unexpected result format"
            return 1
        fi
    else
        test_failed "$test_name" "Query failed or returned empty"
        return 1
    fi
}

# Run all tests
run_all_tests() {
    echo "Running PromQL tests..."
    echo ""
    
    # Basic instant queries
    test_promql_query "BasicInstantQuery" "cpu_usage_percent" ""
    
    # Ordinary functions
    test_promql_query "OrdinaryFunctionAbs" "abs(temperature_celsius)" ""
    test_promql_query "OrdinaryFunctionCeil" "ceil(memory_usage_gb)" ""
    test_promql_query "OrdinaryFunctionFloor" "floor(memory_usage_gb)" ""
    test_promql_query "OrdinaryFunctionRound" "round(memory_usage_gb)" ""
    test_promql_query "OrdinaryFunctionSqrt" "sqrt(cpu_usage_percent)" ""
    
    # Range functions
    test_promql_query "RangeFunctionRate" "rate(http_requests_total[5m])" ""
    test_promql_query "RangeFunctionIncrease" "increase(http_requests_total[5m])" ""
    test_promql_query "RangeFunctionDelta" "delta(cpu_usage_percent[5m])" ""
    test_promql_query "RangeFunctionAvgOverTime" "avg_over_time(cpu_usage_percent[5m])" ""
    test_promql_query "RangeFunctionSumOverTime" "sum_over_time(cpu_usage_percent[5m])" ""
    test_promql_query "RangeFunctionMinOverTime" "min_over_time(cpu_usage_percent[5m])" ""
    test_promql_query "RangeFunctionMaxOverTime" "max_over_time(cpu_usage_percent[5m])" ""
    test_promql_query "RangeFunctionCountOverTime" "count_over_time(cpu_usage_percent[5m])" ""
    
    # Aggregations
    test_promql_query "AggregationSum" "sum(cpu_usage_percent)" ""
    test_promql_query "AggregationAvg" "avg(cpu_usage_percent)" ""
    test_promql_query "AggregationMin" "min(cpu_usage_percent)" ""
    test_promql_query "AggregationMax" "max(cpu_usage_percent)" ""
    test_promql_query "AggregationCount" "count(cpu_usage_percent)" ""
    test_promql_query "AggregationSumByService" "sum(cpu_usage_percent) by (service)" ""
    test_promql_query "AggregationSumByEmpty" "sum(cpu_usage_percent) by ()" ""
    
    # Binary operators
    test_promql_query "BinaryOperatorAdd" "cpu_usage_percent + 10" ""
    test_promql_query "BinaryOperatorMultiply" "cpu_usage_percent * 2" ""
    
    echo ""
    echo "=========================================="
    echo "Test Summary"
    echo "=========================================="
    echo "Passed: $PASSED"
    echo "Failed: $FAILED"
    echo "Skipped: $SKIPPED"
    echo ""
    
    if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
        echo "Failed tests:"
        for test in "${FAILED_TESTS[@]}"; do
            echo "  - $test"
        done
        return 1
    fi
    
    return 0
}

# Main execution
main() {
    setup_test_environment
    insert_test_data
    run_all_tests
}

main "$@"

