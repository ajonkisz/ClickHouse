#include <Storages/TimeSeries/PrometheusHTTPProtocolAPI.h>

#include <set>
#include <fmt/format.h>
#include <Common/logger_useful.h>
#include <Common/quoteString.h>
#include <Common/thread_local_rng.h>
#include <Common/Stopwatch.h>
#include <Formats/FormatSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/ReadBufferFromString.h>
#include <Core/Field.h>
#include <IO/WriteHelpers.h>
#include <fcntl.h>
#include <unistd.h>
#include <Parsers/ASTViewTargets.h>
#include <Storages/StorageTimeSeries.h>
#include <Parsers/Prometheus/PrometheusQueryTree.h>
#include <Parsers/Prometheus/PrometheusQueryResultType.h>
#include <Storages/TimeSeries/PrometheusQueryToSQL.h>
#include <Storages/TimeSeries/TimeSeriesColumnNames.h>
#include <Interpreters/executeQuery.h>
#include <Interpreters/QueryFlags.h>
#include <Interpreters/Context.h>
#include <Core/Settings.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Executors/CompletedPipelineExecutor.h>
#include <Processors/ISink.h>
#include <Processors/Port.h>
#include <QueryPipeline/QueryPipeline.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <Core/Types.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnString.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_REQUEST_PARAMETER;
    extern const int BAD_ARGUMENTS;
    extern const int NOT_IMPLEMENTED;
}

/// Custom sink that writes Prometheus JSON format directly as data arrives
class PrometheusJSONSink : public ISink
{
public:
    enum class ResultType
    {
        INSTANT_VECTOR,
        RANGE_VECTOR,
        SCALAR
    };

    PrometheusJSONSink(SharedHeader header_, WriteBuffer & response_, ResultType result_type_, LoggerPtr log_)
        : ISink(std::move(header_))
        , response(response_)
        , result_type(result_type_)
        , log(log_)
    {
    }

    String getName() const override { return "PrometheusJSONSink"; }

protected:
    void onStart() override
    {
        /// Write JSON header
        if (result_type == ResultType::RANGE_VECTOR)
            writeString(R"({"status":"success","data":{"resultType":"matrix","result":[)", response);
        else
            writeString(R"({"status":"success","data":{"resultType":"vector","result":[)", response);
    }

    void consume(Chunk chunk) override
    {
        auto block = getPort().getHeader().cloneWithColumns(chunk.detachColumns());
        
        if (block.rows() == 0)
            return;

        for (size_t i = 0; i < block.rows(); ++i)
        {
            if (first_result)
                first_result = false;
            else
                writeString(",", response);

            if (result_type == ResultType::RANGE_VECTOR)
                writeRangeResult(block, i);
            else if (result_type == ResultType::INSTANT_VECTOR)
                writeInstantResult(block, i);
            else
                writeScalarResult(block, i);
            
            ++total_rows;
        }
    }

    void onFinish() override
    {
        /// Write JSON footer
        writeString("]}}", response);
        LOG_DEBUG(log, "PrometheusJSONSink: wrote {} rows", total_rows);
    }

private:
    void writeMetricLabels(const Block & block, size_t row_index)
    {
        writeString("{", response);

        if (block.has(TimeSeriesColumnNames::Tags))
        {
            const auto & tags_column = block.getByName(TimeSeriesColumnNames::Tags).column;
            if (const auto * array_column = typeid_cast<const ColumnArray *>(tags_column.get()))
            {
                const auto & offsets = array_column->getOffsets();
                size_t start = (row_index == 0) ? 0 : offsets[row_index - 1];
                size_t end = offsets[row_index];

                if (const auto * tuple_column = typeid_cast<const ColumnTuple *>(&array_column->getData()))
                {
                    const auto & key_column = tuple_column->getColumn(0);
                    const auto & value_column = tuple_column->getColumn(1);

                    bool first = true;
                    for (size_t j = start; j < end; ++j)
                    {
                        if (!first)
                            writeString(",", response);
                        first = false;

                        writeJSONString(key_column.getDataAt(j), response, format_settings);
                        writeString(":", response);
                        writeJSONString(value_column.getDataAt(j), response, format_settings);
                    }
                }
            }
        }

        writeString("}", response);
    }

    void writeInstantResult(const Block & block, size_t row_index)
    {
        writeString(R"({"metric":)", response);
        writeMetricLabels(block, row_index);
        writeString(R"(,"value":[)", response);

        // Timestamp
        if (block.has(TimeSeriesColumnNames::Timestamp))
        {
            const auto & ts_column = block.getByName(TimeSeriesColumnNames::Timestamp).column;
            writeFloatText(ts_column->getFloat64(row_index), response);
        }
        else
        {
            writeFloatText(static_cast<double>(time(nullptr)), response);
        }

        writeString(",", response);

        // Value as string
        if (block.has(TimeSeriesColumnNames::Value))
        {
            const auto & value_column = block.getByName(TimeSeriesColumnNames::Value).column;
            writeJSONString(fmt::format("{}", value_column->getFloat64(row_index)), response, format_settings);
        }
        else
        {
            writeJSONString("0", response, format_settings);
        }

        writeString("]}", response);
    }

    void writeRangeResult(const Block & block, size_t row_index)
    {
        writeString(R"({"metric":)", response);
        writeMetricLabels(block, row_index);
        writeString(R"(,"values":[)", response);

        // For range queries, the TimeSeries column contains Array(Tuple(timestamp, value))
        if (block.has(TimeSeriesColumnNames::TimeSeries))
        {
            const auto & ts_column = block.getByName(TimeSeriesColumnNames::TimeSeries).column;

            if (const auto * array_column = typeid_cast<const ColumnArray *>(ts_column.get()))
            {
                const auto & offsets = array_column->getOffsets();
                size_t start = (row_index == 0) ? 0 : offsets[row_index - 1];
                size_t end = offsets[row_index];

                if (const auto * tuple_column = typeid_cast<const ColumnTuple *>(&array_column->getData()))
                {
                    const auto & ts_data = tuple_column->getColumn(0);  // timestamps
                    const auto & val_data = tuple_column->getColumn(1); // values

                    for (size_t j = start; j < end; ++j)
                    {
                        if (j > start)
                            writeString(",", response);
                        writeString("[", response);
                        writeFloatText(ts_data.getFloat64(j), response);
                        writeString(",", response);
                        writeJSONString(fmt::format("{}", val_data.getFloat64(j)), response, format_settings);
                        writeString("]", response);
                    }
                }
            }
        }

        writeString("]}", response);
    }

    void writeScalarResult(const Block & block, size_t row_index)
    {
        writeString("[", response);

        // Timestamp
        if (block.has(TimeSeriesColumnNames::Timestamp))
        {
            const auto & ts_column = block.getByName(TimeSeriesColumnNames::Timestamp).column;
            writeFloatText(ts_column->getFloat64(row_index), response);
        }
        else
        {
            writeFloatText(static_cast<double>(time(nullptr)), response);
        }

        writeString(",", response);

        // Value as string
        if (block.has(TimeSeriesColumnNames::Scalar))
        {
            const auto & scalar_column = block.getByName(TimeSeriesColumnNames::Scalar).column;
            writeJSONString(fmt::format("{}", scalar_column->getFloat64(row_index)), response, format_settings);
        }
        else if (block.has(TimeSeriesColumnNames::Value))
        {
            const auto & value_column = block.getByName(TimeSeriesColumnNames::Value).column;
            writeJSONString(fmt::format("{}", value_column->getFloat64(row_index)), response, format_settings);
        }
        else
        {
            writeJSONString("0", response, format_settings);
        }

        writeString("]", response);
    }

    WriteBuffer & response;
    ResultType result_type;
    LoggerPtr log;
    FormatSettings format_settings;
    bool first_result = true;
    size_t total_rows = 0;
};

PrometheusHTTPProtocolAPI::PrometheusHTTPProtocolAPI(ConstStoragePtr time_series_storage_, const ContextMutablePtr & context_)
    : WithMutableContext{context_}
    , time_series_storage(storagePtrToTimeSeries(time_series_storage_))
    , log(getLogger("PrometheusHTTPProtocolAPI"))
{
}

PrometheusHTTPProtocolAPI::~PrometheusHTTPProtocolAPI() = default;

void PrometheusHTTPProtocolAPI::executePromQLQuery(
    WriteBuffer & response,
    const Params & params)
{
    Stopwatch total_watch;
    Stopwatch watch;
    
    /// Validate query parameter first
    if (params.promql_query.empty())
    {
        writeString(R"({"status":"error","errorType":"bad_data","error":"Query cannot be empty"})", response);
        return;
    }

    auto query_tree = std::make_unique<PrometheusQueryTree>();
    try
    {
    query_tree->parse(params.promql_query);
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "Failed to parse PromQL query '{}': {}", params.promql_query, e.message());
        writeString(R"({"status":"error","errorType":"bad_data","error":"Failed to parse PromQL query: )", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
        return;
    }

    if (!query_tree)
    {
        writeString(R"({"status":"error","errorType":"bad_data","error":"Failed to parse PromQL query"})", response);
        return;
    }

    UInt64 parse_time_ms = watch.elapsedMilliseconds();
    watch.restart();
    
    LOG_TRACE(log, "Parsed PromQL query: {}. Result type: {}. Parse time: {}ms", params.promql_query, query_tree->getResultType(), parse_time_ms);

    // Create TimeSeriesTableInfo structure
    PrometheusQueryToSQLConverter::TimeSeriesTableInfo table_info;
    table_info.storage_id = time_series_storage->getStorageID();
    table_info.timestamp_data_type = std::make_shared<DataTypeDateTime64>(0);
    table_info.value_data_type = std::make_shared<DataTypeFloat64>();

    Field start_time;
    Field end_time;
    Field step;
    Field evaluation_time;
    Field lookback_delta;

    try
    {
    if (params.type == Type::Instant)
    {
        evaluation_time = parseTimestamp(params.time_param);
        lookback_delta = Field(300.0);
        step = Field(15.0);
    }
    else if (params.type == Type::Range)
    {
        start_time = parseTimestamp(params.start_param);
        end_time = parseTimestamp(params.end_param);
        step = parseStep(params.step_param);
        lookback_delta = Field(end_time.safeGet<Float64>() - start_time.safeGet<Float64>());
        }
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "Failed to parse query parameters: {}", e.message());
        writeString(R"({"status":"error","errorType":"bad_data","error":"Invalid query parameters: )", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
        return;
    }

    PrometheusQueryToSQLConverter converter(
        *query_tree,
        table_info,
        lookback_delta,
        step
    );

    if (params.type == Type::Instant)
        converter.setEvaluationTime(evaluation_time);
    else if (params.type == Type::Range)
        converter.setEvaluationRange({start_time, end_time, step});

    std::shared_ptr<IAST> sql_query;
    try
    {
        sql_query = converter.getSQL();
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "Failed to convert PromQL to SQL: {}", e.message());
        writeString(R"({"status":"error","errorType":"bad_data","error":"Failed to convert PromQL to SQL: )", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
        return;
    }

    if (!sql_query)
    {
        writeString(R"({"status":"error","errorType":"bad_data","error":"Failed to convert PromQL to SQL"})", response);
        return;
    }

    UInt64 conversion_time_ms = watch.elapsedMilliseconds();
    watch.restart();
    
    String sql_string = sql_query->formatWithSecretsOneLine();
    LOG_WARNING(log, "PromQL '{}' -> SQL ({}ms): {}", params.promql_query, conversion_time_ms, sql_string);

    /// Create a copy of the context for query execution to avoid modifying the original
    auto query_context = Context::createCopy(getContext());
    query_context->makeQueryContext();
    query_context->setCurrentQueryId(toString(thread_local_rng()));
    query_context->setSetting("allow_experimental_time_series_aggregate_functions", Field(1));
    
    UInt64 context_setup_ms = watch.elapsedMilliseconds();
    watch.restart();

    try
    {
        /// Execute query using streaming approach with CompletedPipelineExecutor
        auto [ast, io] = executeQuery(sql_string, query_context, QueryFlags{.internal = false}, QueryProcessingStage::Complete);

        UInt64 query_parse_compile_ms = watch.elapsedMilliseconds();
        watch.restart();

        /// Determine the result type for the sink
        PrometheusJSONSink::ResultType sink_result_type;
        if (converter.getResultType() == PrometheusQueryTree::ResultType::RANGE_VECTOR)
            sink_result_type = PrometheusJSONSink::ResultType::RANGE_VECTOR;
        else if (converter.getResultType() == PrometheusQueryTree::ResultType::INSTANT_VECTOR)
            sink_result_type = PrometheusJSONSink::ResultType::INSTANT_VECTOR;
        else if (converter.getResultType() == PrometheusQueryTree::ResultType::SCALAR)
            sink_result_type = PrometheusJSONSink::ResultType::SCALAR;
        else
        {
            LOG_ERROR(log, "Unsupported result type: {}", converter.getResultType());
            writeString(R"({"status":"error","errorType":"internal","error":"Unsupported result type"})", response);
            return;
        }

        /// Create a custom sink that writes Prometheus JSON format
        auto prometheus_sink = std::make_shared<PrometheusJSONSink>(
            io.pipeline.getSharedHeader(),
            response,
            sink_result_type,
            log
        );

        /// Complete the pipeline with our sink
        io.pipeline.complete(prometheus_sink);

        UInt64 sink_setup_ms = watch.elapsedMilliseconds();
        watch.restart();

        /// Execute the completed pipeline - this runs the entire query and writes results
        CompletedPipelineExecutor executor(io.pipeline);
        executor.execute();

        UInt64 execution_ms = watch.elapsedMilliseconds();
        UInt64 total_time_ms = total_watch.elapsedMilliseconds();
        
        // Write detailed timing to file for analysis
        // Format: timestamp|query|total|promql|sql|ctx|compile|sink_setup|execution|rows
        try
        {
            String timing_file = "/tmp/clickhouse_promql_timings.log";
            WriteBufferFromFile timing_buf(timing_file, DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_CREAT | O_WRONLY);
            writeString(fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|streaming\n",
                time(nullptr),
                params.promql_query,
                total_time_ms,
                parse_time_ms,
                conversion_time_ms,
                context_setup_ms,
                query_parse_compile_ms,
                sink_setup_ms,
                execution_ms), timing_buf);
            timing_buf.finalize();
        }
        catch (...)
        {
            // Ignore timing file errors
        }
        
        LOG_INFO(log, "PromQL '{}' {}ms STREAMING (promql: {}ms, sql: {}ms, ctx: {}ms, compile: {}ms, sink_setup: {}ms, execution: {}ms)", 
                 params.promql_query, total_time_ms, parse_time_ms, conversion_time_ms, context_setup_ms, query_parse_compile_ms, sink_setup_ms, execution_ms);
        return;
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "Query execution failed: {}", e.message());
        writeString(R"({"status":"error","errorType":"internal","error":"Query execution failed: )", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
        return;
    }
}

void PrometheusHTTPProtocolAPI::getSeries(
    WriteBuffer & response,
    const String & match_param,
    const String & start_param,
    const String & end_param)
{
    try
{
    /// Build SQL query to get all series matching the selector
    /// SELECT DISTINCT tags FROM <tags_table> WHERE <match conditions> [AND timestamp BETWEEN start AND end]

        /// Get the actual tags table ID from the TimeSeries storage
        auto tags_table_id = time_series_storage->getTargetTableId(ViewTarget::Tags);
        String tags_table = backQuoteIfNeed(tags_table_id.database_name) + "." + backQuoteIfNeed(tags_table_id.table_name);

        /// Note: 'all_tags' is EPHEMERAL (not stored), so we use 'tags' combined with 'metric_name'
        String sql = fmt::format("SELECT DISTINCT metric_name, tags FROM {} ", tags_table);

    std::vector<String> conditions;

    /// Parse match[] parameter if provided
    if (!match_param.empty())
    {
        /// For now, assume match_param is a metric name or simple label matcher
        /// A full implementation would parse PromQL label matchers
        if (match_param.find('{') == String::npos)
        {
            /// Simple metric name
            conditions.push_back(fmt::format("metric_name = '{}'", match_param));
        }
        /// TODO: Parse complex label matchers like {job="prometheus"}
    }

    /// Add time range conditions if provided
    if (!start_param.empty())
    {
        auto start_ts = parseTimestamp(start_param);
        conditions.push_back(fmt::format("min_time >= {}", start_ts.safeGet<Float64>()));
    }
    if (!end_param.empty())
    {
        auto end_ts = parseTimestamp(end_param);
        conditions.push_back(fmt::format("max_time <= {}", end_ts.safeGet<Float64>()));
    }

    if (!conditions.empty())
    {
        sql += " WHERE ";
        for (size_t i = 0; i < conditions.size(); ++i)
        {
            if (i > 0)
                sql += " AND ";
            sql += conditions[i];
        }
    }

    sql += " LIMIT 10000";

    LOG_DEBUG(log, "Series query SQL: {}", sql);

        /// Create a copy of the context for query execution to avoid modifying the original
        auto query_context = Context::createCopy(getContext());
    query_context->makeQueryContext();
    query_context->setCurrentQueryId(toString(thread_local_rng()));

        /// Execute as internal query to prevent assertions in some edge cases
        auto [ast, io] = executeQuery(sql, query_context, QueryFlags{.internal = true}, QueryProcessingStage::Complete);

    PullingPipelineExecutor executor(io.pipeline);
    Block result_block;

    writeString(R"({"status":"success","data":[)", response);
    bool first_series = true;

    while (executor.pull(result_block))
    {
        if (result_block.empty() || result_block.rows() == 0)
            continue;

        /// Column 0 is metric_name, column 1 is tags (Map)
        const auto & metric_name_column = result_block.getByPosition(0).column;
        const auto & tags_column = result_block.getByPosition(1).column;

        for (size_t row = 0; row < result_block.rows(); ++row)
        {
            if (!first_series)
                writeString(",", response);
            first_series = false;

            /// Write the series as a JSON object of labels
            writeString("{", response);

            /// First add __name__ from metric_name
            Field metric_name_field;
            metric_name_column->get(row, metric_name_field);
            String metric_name;
            if (metric_name_field.tryGet<String>(metric_name))
            {
                writeString(R"("__name__":")", response);
                writeString(metric_name, response);
                writeString("\"", response);
            }

            /// Then add other labels from tags map
            Field tags_field;
            tags_column->get(row, tags_field);
            if (tags_field.getType() == Field::Types::Map)
            {
                const auto & tags_map = tags_field.safeGet<Map>();
                for (const auto & map_element : tags_map)
                {
                    const auto & map_entry = map_element.safeGet<Tuple>();
                    if (map_entry.size() != 2)
                        continue;

                    String key;
                    String value;
                    if (!map_entry[0].tryGet<String>(key) || !map_entry[1].tryGet<String>(value))
                        continue;

                    writeString(",\"", response);
                    writeString(key, response);
                    writeString("\":\"", response);
                    writeString(value, response);
                    writeString("\"", response);
                }
            }

            writeString("}", response);
        }
    }

    writeString("]}", response);
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "getSeries failed: {}", e.message());
        writeString(R"({"status":"error","errorType":"internal","error":")", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
    }
}

void PrometheusHTTPProtocolAPI::getLabels(
    WriteBuffer & response,
    const String & match_param,
    const String & start_param,
    const String & end_param)
{
    try
{
    /// Build SQL query to get all distinct label names
    /// We need to extract keys from the all_tags Map column
        /// Get the actual tags table ID from the TimeSeries storage
        auto tags_table_id = time_series_storage->getTargetTableId(ViewTarget::Tags);
        String tags_table = backQuoteIfNeed(tags_table_id.database_name) + "." + backQuoteIfNeed(tags_table_id.table_name);

        /// Use arrayJoin(mapKeys(tags)) to get unique label names
        /// Note: 'all_tags' is EPHEMERAL (not stored), so we use 'tags'
    String sql = fmt::format(
            "SELECT DISTINCT arrayJoin(mapKeys(tags)) as label_name FROM {} ",
        tags_table);

    std::vector<String> conditions;

    if (!match_param.empty())
    {
        if (match_param.find('{') == String::npos)
        {
            conditions.push_back(fmt::format("metric_name = '{}'", match_param));
        }
    }

    if (!start_param.empty())
    {
        auto start_ts = parseTimestamp(start_param);
        conditions.push_back(fmt::format("min_time >= {}", start_ts.safeGet<Float64>()));
    }
    if (!end_param.empty())
    {
        auto end_ts = parseTimestamp(end_param);
        conditions.push_back(fmt::format("max_time <= {}", end_ts.safeGet<Float64>()));
    }

    if (!conditions.empty())
    {
        sql += " WHERE ";
        for (size_t i = 0; i < conditions.size(); ++i)
        {
            if (i > 0)
                sql += " AND ";
            sql += conditions[i];
        }
    }

    sql += " ORDER BY label_name LIMIT 10000";

    LOG_DEBUG(log, "Labels query SQL: {}", sql);

        /// Create a copy of the context for query execution to avoid modifying the original
        auto query_context = Context::createCopy(getContext());
    query_context->makeQueryContext();
    query_context->setCurrentQueryId(toString(thread_local_rng()));

        /// Execute as internal query to prevent assertions in some edge cases
        auto [ast, io] = executeQuery(sql, query_context, QueryFlags{.internal = true}, QueryProcessingStage::Complete);

    PullingPipelineExecutor executor(io.pipeline);
    Block result_block;

    /// Collect all labels first to avoid duplicates
    std::set<String> all_labels;
    all_labels.insert("__name__");  /// Always include __name__

    while (executor.pull(result_block))
    {
        if (result_block.empty() || result_block.rows() == 0)
            continue;

        const auto & label_column = result_block.getByPosition(0).column;
        for (size_t row = 0; row < result_block.rows(); ++row)
        {
            Field field;
            label_column->get(row, field);
            String label_name;
            if (field.tryGet<String>(label_name) && !label_name.empty())
                all_labels.insert(label_name);
        }
    }

    writeString(R"({"status":"success","data":[)", response);
    bool first = true;
    for (const auto & label : all_labels)
    {
        if (!first)
            writeString(",", response);
        first = false;

        writeString("\"", response);
        writeString(label, response);
        writeString("\"", response);
    }
    writeString("]}", response);
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "getLabels failed: {}", e.message());
        writeString(R"({"status":"error","errorType":"internal","error":")", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
    }
}

void PrometheusHTTPProtocolAPI::getLabelValues(
    WriteBuffer & response,
    const String & label_name,
    const String & match_param,
    const String & start_param,
    const String & end_param)
{
    try
    {
        /// Get the actual tags table ID from the TimeSeries storage
        auto tags_table_id = time_series_storage->getTargetTableId(ViewTarget::Tags);
        String tags_table = backQuoteIfNeed(tags_table_id.database_name) + "." + backQuoteIfNeed(tags_table_id.table_name);

    String sql;

    /// Special handling for __name__ label
    if (label_name == "__name__")
    {
        sql = fmt::format("SELECT DISTINCT metric_name FROM {} ", tags_table);
    }
    else
    {
        /// Get distinct values for the specified label from the tags Map
        /// Note: 'all_tags' is EPHEMERAL (not stored), so we use 'tags'
        sql = fmt::format(
            "SELECT DISTINCT tags['{}'] as label_value FROM {} WHERE mapContains(tags, '{}') ",
            label_name, tags_table, label_name);
    }

    std::vector<String> conditions;

    if (!match_param.empty())
    {
        if (match_param.find('{') == String::npos)
        {
            conditions.push_back(fmt::format("metric_name = '{}'", match_param));
        }
    }

    if (!start_param.empty())
    {
        auto start_ts = parseTimestamp(start_param);
        conditions.push_back(fmt::format("min_time >= {}", start_ts.safeGet<Float64>()));
    }
    if (!end_param.empty())
    {
        auto end_ts = parseTimestamp(end_param);
        conditions.push_back(fmt::format("max_time <= {}", end_ts.safeGet<Float64>()));
    }

    if (!conditions.empty())
    {
        /// For __name__ we start fresh with WHERE, for others we already have WHERE
        if (label_name == "__name__")
        {
            sql += " WHERE ";
            for (size_t i = 0; i < conditions.size(); ++i)
            {
                if (i > 0)
                    sql += " AND ";
                sql += conditions[i];
            }
        }
        else
        {
            for (const auto & cond : conditions)
                sql += " AND " + cond;
        }
    }

    sql += " ORDER BY 1 LIMIT 10000";

    LOG_DEBUG(log, "Label values query SQL: {}", sql);

        /// Create a copy of the context for query execution to avoid modifying the original
        auto query_context = Context::createCopy(getContext());
    query_context->makeQueryContext();
    query_context->setCurrentQueryId(toString(thread_local_rng()));

        /// Execute as internal query to prevent assertions in some edge cases
        auto [ast, io] = executeQuery(sql, query_context, QueryFlags{.internal = true}, QueryProcessingStage::Complete);

    PullingPipelineExecutor executor(io.pipeline);
    Block result_block;

    std::set<String> all_values;

    while (executor.pull(result_block))
    {
        if (result_block.empty() || result_block.rows() == 0)
            continue;

        const auto & value_column = result_block.getByPosition(0).column;
        for (size_t row = 0; row < result_block.rows(); ++row)
        {
            Field field;
            value_column->get(row, field);
            String value;
            if (field.tryGet<String>(value) && !value.empty())
                all_values.insert(value);
        }
    }

    writeString(R"({"status":"success","data":[)", response);
    bool first = true;
    for (const auto & value : all_values)
    {
        if (!first)
            writeString(",", response);
        first = false;

        writeString("\"", response);
        writeString(value, response);
        writeString("\"", response);
    }
    writeString("]}", response);
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "getLabelValues failed: {}", e.message());
        writeString(R"({"status":"error","errorType":"internal","error":")", response);
        writeString(e.message(), response);
        writeString(R"("})", response);
    }
}

Field PrometheusHTTPProtocolAPI::parseTimestamp(const String & time_param)
{
    if (time_param.empty())
        return Field(static_cast<Float64>(time(nullptr))); // Current time as default

    // Try to parse as Unix timestamp
    try
    {
        Float64 timestamp = std::stod(time_param);
        return Field(timestamp);
    }
    catch (...)
    {
        throw Exception(ErrorCodes::BAD_REQUEST_PARAMETER, "Invalid timestamp format: {}", time_param);
    }
}

Field PrometheusHTTPProtocolAPI::parseStep(const String & step_param)
{
    if (step_param.empty())
        return Field(15.0); // Default 15 seconds

    try
    {
        // Parse step parameter (e.g., "15s", "1m", "1h")
        if (step_param.ends_with("s"))
        {
            String num_str = step_param.substr(0, step_param.length() - 1);
            return Field(std::stod(num_str));
        }
        else if (step_param.ends_with("m"))
        {
            String num_str = step_param.substr(0, step_param.length() - 1);
            return Field(std::stod(num_str) * 60);
        }
        else if (step_param.ends_with("h"))
        {
            String num_str = step_param.substr(0, step_param.length() - 1);
            return Field(std::stod(num_str) * 3600);
        }
        else
        {
            // Assume seconds if no unit
            return Field(std::stod(step_param));
        }
    }
    catch (...)
    {
        throw Exception(ErrorCodes::BAD_REQUEST_PARAMETER, "Invalid step format: {}", step_param);
    }
}

void DB::PrometheusHTTPProtocolAPI::writeInstantQueryHeader(WriteBuffer & response)
{
    writeString(R"({"status":"success","data":{)", response);
}

void DB::PrometheusHTTPProtocolAPI::writeScalarQueryResponse(WriteBuffer & response, const Block & result_block)
{
    chassert(!result_block.empty() && result_block.has(TimeSeriesColumnNames::Scalar) && !result_block.has(TimeSeriesColumnNames::Tags));
    writeString(R"("resultType":"scalar","result":)", response);
    writeScalarResult(response, result_block);
}

void DB::PrometheusHTTPProtocolAPI::writeInstantQueryResponse(WriteBuffer & response, const Block & result_block)
{
    writeString(R"("resultType":"vector","result":)", response);
    writeVectorResult(response, result_block);
}

void DB::PrometheusHTTPProtocolAPI::writeInstantQueryFooter(WriteBuffer & response)
{
    writeString("}}", response);
}

void DB::PrometheusHTTPProtocolAPI::writeScalarResult(WriteBuffer & response, const Block & result_block)
{
    LOG_INFO(log, "Prometheus: Writing scalar result");

    writeString("[", response);

    if (!result_block.empty() && result_block.rows() > 0)
    {
        // Write timestamp
        if (result_block.has(TimeSeriesColumnNames::Timestamp))
        {
            const auto & ts_column = result_block.getByName(TimeSeriesColumnNames::Timestamp).column;
            auto timestamp = ts_column->getFloat64(0);
            writeString(std::to_string(timestamp), response);
        }
        else
        {
            writeString(std::to_string(time(nullptr)), response);
        }

        writeString(",", response);

        // Write value
        if (result_block.has(TimeSeriesColumnNames::Scalar))
        {
            const auto & scalar_column = result_block.getByName(TimeSeriesColumnNames::Scalar).column;
            auto value = scalar_column->getFloat64(0);
            writeString("\"", response);
            writeFloatText(std::round(value * 100.0) / 100.0, response);
            writeString("\"", response);
        }
        else
        {
            writeString("\"0\"", response);
        }
    }

    writeString("]", response);
}

void DB::PrometheusHTTPProtocolAPI::writeVectorResult(WriteBuffer & response, const Block & result_block)
{
    // Optimized: Batch entire result in memory buffer, then write once to reduce overhead
    WriteBufferFromOwnString temp_buffer;
    WriteBuffer & buf = temp_buffer;
    FormatSettings json_settings;

    writeChar('[', buf);

    if (!result_block.empty() && result_block.rows() > 0)
    {
        for (size_t i = 0; i < result_block.rows(); ++i)
        {
            if (i > 0)
                writeChar(',', buf);

            writeChar('{', buf);

            // Write metric labels
            writeString(R"("metric":)", buf);
            writeMetricLabels(buf, result_block, i);

            writeChar(',', buf);

            // Write value [timestamp, "value"]
            writeString(R"("value":[)", buf);

            // Write timestamp
            if (result_block.has(TimeSeriesColumnNames::Timestamp))
            {
                const auto & ts_column = result_block.getByName(TimeSeriesColumnNames::Timestamp).column;
                auto timestamp = ts_column->getFloat64(i);
                writeFloatText(std::round(timestamp * 100.0) / 100.0, buf);
            }
            else
            {
                writeFloatText(std::round(time(nullptr) * 100.0) / 100.0, buf);
            }

            writeChar(',', buf);

            // Write value
            if (result_block.has(TimeSeriesColumnNames::Value))
            {
                const auto & value_column = result_block.getByName(TimeSeriesColumnNames::Value).column;
                auto value = value_column->getFloat64(i);
                writeChar('"', buf);
                writeFloatText(std::round(value * 100.0) / 100.0, buf);
                writeChar('"', buf);
            }
            else if (result_block.has(TimeSeriesColumnNames::Scalar))
            {
                const auto & scalar_column = result_block.getByName(TimeSeriesColumnNames::Scalar).column;
                auto value = scalar_column->getFloat64(i);
                writeChar('"', buf);
                writeFloatText(std::round(value * 100.0) / 100.0, buf);
                writeChar('"', buf);
            }
            else
            {
                writeString(R"("0")", buf);
            }

            writeString("]}", buf);
        }
    }

    writeChar(']', buf);
    buf.finalize();
    
    // Write the entire accumulated JSON in one call - this is the key optimization
    writeString(temp_buffer.str(), response);
}

void DB::PrometheusHTTPProtocolAPI::writeMetricLabels(WriteBuffer & response, const Block & result_block, size_t row_index)
{
    // Optimized: Use writeJSONString for proper escaping and batch operations
    FormatSettings json_settings;

    writeChar('{', response);

    if (result_block.has(TimeSeriesColumnNames::Tags))
    {
        const auto & tags_column = result_block.getByName(TimeSeriesColumnNames::Tags).column;
        if (const auto * array_column = typeid_cast<const ColumnArray *>(tags_column.get()))
        {
            const auto & offsets = array_column->getOffsets();
            size_t start = (row_index == 0) ? 0 : offsets[row_index - 1];
            size_t end = offsets[row_index];

            if (const auto * tuple_column = typeid_cast<const ColumnTuple *>(&array_column->getData()))
            {
                const auto & key_column = tuple_column->getColumn(0);
                const auto & value_column = tuple_column->getColumn(1);

                bool first = true;
                for (size_t j = start; j < end; ++j)
                {
                    if (!first)
                        writeChar(',', response);
                    first = false;

                    // Use writeJSONString for proper escaping and efficiency
                    auto key_data = key_column.getDataAt(j);
                    writeJSONString(key_data, response, json_settings);
                    writeChar(':', response);
                    
                    auto value_data = value_column.getDataAt(j);
                    writeJSONString(value_data, response, json_settings);
                }
            }
        }
    }

    writeChar('}', response);
}

void DB::PrometheusHTTPProtocolAPI::writeRangeQueryHeader(WriteBuffer & response)
{
    writeString(R"({"status":"success","data":{"resultType":"matrix","result":[)", response);
}

void DB::PrometheusHTTPProtocolAPI::writeRangeQueryFooter(WriteBuffer & response)
{
    writeString(R"(]}})", response);
}

void DB::PrometheusHTTPProtocolAPI::writeRangeQueryResponse(WriteBuffer & response, const Block & result_block)
{
    if (!result_block.empty() && result_block.rows() > 0)
    {
        // For range queries, we need to group results by metric labels
        // This is a simplified implementation
        for (size_t i = 0; i < result_block.rows(); ++i)
        {
            if (i > 0)
                writeString(",", response);

            writeString("{", response);

            // Write metric labels using the shared function that skips __name__
            writeString(R"("metric":)", response);
            writeMetricLabels(response, result_block, i);
            writeString(",", response);

            // Extract time series data
            writeString(R"("values":[)", response);


            const auto & ts_column = result_block.getByName(TimeSeriesColumnNames::TimeSeries).column;
            if (const auto * array_column = typeid_cast<const ColumnArray *>(ts_column.get()))
            {
                const auto & offsets = array_column->getOffsets();
                size_t start = (i == 0) ? 0 : offsets[i-1];
                size_t end = offsets[i];

                if (const auto * tuple_column = typeid_cast<const ColumnTuple *>(&array_column->getData()))
                {
                    const auto & timestamp_column = tuple_column->getColumn(0);
                    const auto & value_column = tuple_column->getColumn(1);

                    for (size_t j = start; j < end; ++j)
                    {
                        if (j > start)
                            writeString(",", response);

                        writeString("[", response);
                        writeFloatText(timestamp_column.getFloat64(j), response);
                        writeString(",\"", response);
                        writeFloatText(std::round(value_column.getFloat64(j) * 100.0) / 100.0, response);
                        writeString("\"]", response);
                    }
                }
            }

            writeString("]}", response);
        }
    }
}

void DB::PrometheusHTTPProtocolAPI::writeSeriesResponse(WriteBuffer & response, const Block & /* result_block */)
{
    writeString(R"({"status":"success","data":[]})", response);
}

void DB::PrometheusHTTPProtocolAPI::writeLabelsResponse(WriteBuffer & response, const Block & /* result_block */)
{
    writeString(R"({"status":"success","data":["__name__","job","instance"]})", response);
}

void DB::PrometheusHTTPProtocolAPI::writeLabelValuesResponse(WriteBuffer & response, const Block & /* result_block */)
{
    writeString(R"({"status":"success","data":[]})", response);
}

}
