#include <Storages/TimeSeries/TimeSeriesSink.h>

#include <Columns/ColumnMap.h>
#include <Columns/ColumnNullable.h>
#include <Common/logger_useful.h>
#include <Core/Block.h>
#include <Core/ColumnsWithTypeAndName.h>
#include <Core/DecimalFunctions.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/InterpreterInsertQuery.h>
#include <Interpreters/addMissingDefaults.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Processors/Chunk.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Executors/PushingPipelineExecutor.h>
#include <Processors/Sources/BlocksSource.h>
#include <Processors/Transforms/ExpressionTransform.h>
#include <QueryPipeline/BlockIO.h>
#include <QueryPipeline/Pipe.h>
#include <Storages/IStorage.h>
#include <Storages/StorageTimeSeries.h>
#include <Storages/TimeSeries/TimeSeriesColumnNames.h>
#include <Storages/TimeSeries/TimeSeriesSettings.h>


namespace DB
{

namespace TimeSeriesSetting
{
    extern const TimeSeriesSettingsBool store_min_time_and_max_time;
    extern const TimeSeriesSettingsMap tags_to_columns;
    extern const TimeSeriesSettingsBool use_all_tags_column_to_generate_id;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{
    /// Calculates the identifier using the default expression for the "id" column,
    /// and returns the calculated column.
    ColumnPtr calculateId(const ContextPtr & context, const ColumnDescription & id_column_description, Block & tags_block)
    {
        auto blocks = std::make_shared<Blocks>();
        blocks->push_back(tags_block);

        auto header = std::make_shared<const Block>(tags_block.cloneEmpty());
        auto pipe = Pipe(std::make_shared<BlocksSource>(blocks, header));

        Block header_with_id;
        const auto & id_name = id_column_description.name;
        auto id_type = id_column_description.type;
        header_with_id.insert(ColumnWithTypeAndName{id_type, id_name});

        auto adding_missing_defaults_dag = addMissingDefaults(
            pipe.getHeader(),
            header_with_id.getNamesAndTypesList(),
            ColumnsDescription{id_column_description},
            context);

        auto adding_missing_defaults_actions = std::make_shared<ExpressionActions>(std::move(adding_missing_defaults_dag));
        pipe.addSimpleTransform([&](const SharedHeader & stream_header)
        {
            return std::make_shared<ExpressionTransform>(stream_header, adding_missing_defaults_actions);
        });

        auto convert_actions_dag = ActionsDAG::makeConvertingActions(
            pipe.getHeader().getColumnsWithTypeAndName(),
            header_with_id.getColumnsWithTypeAndName(),
            ActionsDAG::MatchColumnsMode::Position,
            context);
        auto actions = std::make_shared<ExpressionActions>(
            std::move(convert_actions_dag),
            ExpressionActionsSettings(context, CompileExpressions::yes));
        pipe.addSimpleTransform([&](const SharedHeader & stream_header)
        {
            return std::make_shared<ExpressionTransform>(stream_header, actions);
        });

        QueryPipeline pipeline{std::move(pipe)};
        PullingPipelineExecutor executor{pipeline};

        MutableColumnPtr id_column;

        Block block_from_executor;
        while (executor.pull(block_from_executor))
        {
            if (!block_from_executor.empty())
            {
                MutableColumnPtr id_column_part = block_from_executor.getByName(id_name).column->assumeMutable();
                if (id_column)
                    id_column->insertRangeFrom(*id_column_part, 0, id_column_part->size());
                else
                    id_column = std::move(id_column_part);
            }
        }

        if (!id_column)
            id_column = id_type->createColumn();

        return id_column;
    }
}


TimeSeriesSink::TimeSeriesSink(
    StorageTimeSeries & storage_,
    const StorageMetadataPtr & metadata_snapshot_,
    ContextPtr context_)
    : SinkToStorage(std::make_shared<const Block>(metadata_snapshot_->getSampleBlock()))
    , WithContext(context_)
    , storage(storage_)
    , metadata_snapshot(metadata_snapshot_)
    , time_series_settings(std::make_shared<TimeSeriesSettings>(storage_.getStorageSettings()))
    , log(getLogger("TimeSeriesSink"))
{
}

void TimeSeriesSink::consume(Chunk & chunk)
{
    size_t rows = chunk.getNumRows();
    if (!rows)
        return;

    auto block = getHeader().cloneWithColumns(chunk.getColumns());
    splitBlock(block);
}

void TimeSeriesSink::splitBlock(const Block & block)
{
    const auto & columns_description = metadata_snapshot->columns;
    const size_t num_rows = block.rows();

    if (!num_rows)
        return;

    /// Get the ID column description (needed for ID calculation)
    const auto & id_description = columns_description.get(TimeSeriesColumnNames::ID);

    /// Build data block (id, timestamp, value)
    Block data_block;
    {
        /// Get timestamp column
        if (block.has(TimeSeriesColumnNames::Timestamp))
        {
            auto timestamp_col = block.getByName(TimeSeriesColumnNames::Timestamp);
            data_block.insert(timestamp_col);
        }

        /// Get value column
        if (block.has(TimeSeriesColumnNames::Value))
        {
            auto value_col = block.getByName(TimeSeriesColumnNames::Value);
            data_block.insert(value_col);
        }
    }

    /// Build tags block (metric_name, tags, all_tags for ID calculation)
    Block tags_block;
    {
        /// Get metric_name column
        if (block.has(TimeSeriesColumnNames::MetricName))
        {
            auto metric_name_col = block.getByName(TimeSeriesColumnNames::MetricName);
            tags_block.insert(metric_name_col);
        }

        /// Get tags column
        if (block.has(TimeSeriesColumnNames::Tags))
        {
            auto tags_col = block.getByName(TimeSeriesColumnNames::Tags);
            tags_block.insert(tags_col);
        }

        /// Get or build all_tags column for ID calculation
        if ((*time_series_settings)[TimeSeriesSetting::use_all_tags_column_to_generate_id])
        {
            if (block.has(TimeSeriesColumnNames::AllTags))
            {
                auto all_tags_col = block.getByName(TimeSeriesColumnNames::AllTags);
                tags_block.insert(all_tags_col);
            }
        }

        /// Get additional tag columns specified in tags_to_columns setting
        const Map & tags_to_columns = (*time_series_settings)[TimeSeriesSetting::tags_to_columns];
        for (const auto & tag_name_and_column_name : tags_to_columns)
        {
            const auto & tuple = tag_name_and_column_name.safeGet<Tuple>();
            const auto & column_name = tuple.at(1).safeGet<String>();
            if (block.has(column_name))
            {
                ColumnWithTypeAndName tag_col = block.getByName(column_name);
                tags_block.insert(tag_col);
            }
        }

        /// Get min_time and max_time columns if present
        if ((*time_series_settings)[TimeSeriesSetting::store_min_time_and_max_time])
        {
            if (block.has(TimeSeriesColumnNames::MinTime))
            {
                auto min_time_col = block.getByName(TimeSeriesColumnNames::MinTime);
                tags_block.insert(min_time_col);
            }
            if (block.has(TimeSeriesColumnNames::MaxTime))
            {
                auto max_time_col = block.getByName(TimeSeriesColumnNames::MaxTime);
                tags_block.insert(max_time_col);
            }
        }
    }

    /// Calculate or get the ID column
    ColumnPtr id_column;
    if (block.has(TimeSeriesColumnNames::ID))
    {
        /// User provided the ID column directly
        id_column = block.getByName(TimeSeriesColumnNames::ID).column;
    }
    else
    {
        /// Calculate ID from metric_name and tags using the default expression
        id_column = calculateId(getContext(), id_description, tags_block);
    }

    /// Add ID column to data block
    data_block.insert(ColumnWithTypeAndName{id_column, id_description.type, TimeSeriesColumnNames::ID});

    /// Add ID column to tags block (at the beginning)
    tags_block.insert(0, ColumnWithTypeAndName{id_column, id_description.type, TimeSeriesColumnNames::ID});

    /// Remove all_tags column from tags_block before inserting (it's ephemeral)
    if (tags_block.has(TimeSeriesColumnNames::AllTags))
        tags_block.erase(TimeSeriesColumnNames::AllTags);

    /// Accumulate blocks - tags first, then data
    /// (Because any INSERT can fail and we don't want to have rows in the data table with no corresponding "id" written to the "tags" table.)
    bool found_tags = false;
    bool found_data = false;
    for (auto & [kind, accumulated_block] : accumulated_blocks)
    {
        if (kind == ViewTarget::Tags)
        {
            /// Append to existing tags block
            MutableColumns cols = accumulated_block.mutateColumns();
            for (size_t i = 0; i < cols.size(); ++i)
            {
                if (tags_block.has(accumulated_block.getByPosition(i).name))
                {
                    const auto & src_col = tags_block.getByName(accumulated_block.getByPosition(i).name);
                    cols[i]->insertRangeFrom(*src_col.column, 0, src_col.column->size());
                }
            }
            accumulated_block.setColumns(std::move(cols));
            found_tags = true;
        }
        else if (kind == ViewTarget::Data)
        {
            /// Append to existing data block
            MutableColumns cols = accumulated_block.mutateColumns();
            for (size_t i = 0; i < cols.size(); ++i)
            {
                if (data_block.has(accumulated_block.getByPosition(i).name))
                {
                    const auto & src_col = data_block.getByName(accumulated_block.getByPosition(i).name);
                    cols[i]->insertRangeFrom(*src_col.column, 0, src_col.column->size());
                }
            }
            accumulated_block.setColumns(std::move(cols));
            found_data = true;
        }
    }

    if (!found_tags)
        accumulated_blocks.emplace_back(ViewTarget::Tags, std::move(tags_block));
    if (!found_data)
        accumulated_blocks.emplace_back(ViewTarget::Data, std::move(data_block));
}

void TimeSeriesSink::onFinish()
{
    insertToTargetTables();
}

void TimeSeriesSink::insertToTargetTables()
{
    auto time_series_storage_id = storage.getStorageID();

    for (auto & [table_kind, block] : accumulated_blocks)
    {
        if (!block.empty() && block.rows() > 0)
        {
            const auto & target_table_id = storage.getTargetTableId(table_kind);

            LOG_INFO(log, "{}: Inserting {} rows to the {} table",
                     time_series_storage_id.getNameForLogs(), block.rows(), toString(table_kind));

            auto insert_query = std::make_shared<ASTInsertQuery>();
            insert_query->table_id = target_table_id;

            auto columns_ast = std::make_shared<ASTExpressionList>();
            for (const auto & name : block.getNames())
                columns_ast->children.emplace_back(std::make_shared<ASTIdentifier>(name));
            insert_query->columns = columns_ast;

            ContextMutablePtr insert_context = Context::createCopy(getContext());
            insert_context->setCurrentQueryId(getContext()->getCurrentQueryId() + ":" + String{toString(table_kind)});

            LOG_TEST(log, "{}: Executing query: {}", time_series_storage_id.getNameForLogs(), insert_query->formatForLogging());

            InterpreterInsertQuery interpreter(
                insert_query,
                insert_context,
                /* allow_materialized= */ false,
                /* no_squash= */ false,
                /* no_destination= */ false,
                /* async_insert= */ false);

            BlockIO io = interpreter.execute();
            PushingPipelineExecutor executor(io.pipeline);

            executor.start();
            executor.push(std::move(block));
            executor.finish();
        }
    }

    accumulated_blocks.clear();
}

}

