#include "storage/delta_insert.hpp"

#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "functions/delta_scan/delta_scan.hpp"
#include "duckdb/execution/physical_operator_states.hpp"

#include "storage/delta_catalog.hpp"
#include "storage/delta_transaction.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "storage/delta_table_entry.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

DeltaInsert::DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table,
                     physical_index_vector_t<idx_t> column_index_map_p)
: PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(&table), schema(nullptr),
  column_index_map(std::move(column_index_map_p)) {
}

DeltaInsert::DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(nullptr), schema(&schema),
      info(std::move(info)) {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class DeltaInsertGlobalState : public GlobalSinkState {
public:
	explicit DeltaInsertGlobalState(const DeltaTableEntry &table) : table_name(table.name), not_null_constraints(table.GetNotNullConstraints()) {
	    table.ThrowOnUnsupportedFieldForInserting();
	};

    string table_name;

    vector<DeltaDataFile> written_files;

    idx_t insert_count = 0;

    // Fields in the table with not null constraints. These
    case_insensitive_map_t<vector<NestedNotNullConstraint>> not_null_constraints;
};

unique_ptr<GlobalSinkState> DeltaInsert::GetGlobalSinkState(ClientContext &context) const {
    // TODO: what if table isn't set?
    const auto &delta_table = table->Cast<DeltaTableEntry>();
	return make_uniq<DeltaInsertGlobalState>(delta_table);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
static string ParseQuotedValue(const string &input, idx_t &pos) {
    if (pos >= input.size() || input[pos] != '"') {
        throw InvalidInputException("Failed to parse quoted value - expected a quote");
    }
    string result;
    pos++;
    for (; pos < input.size(); pos++) {
        if (input[pos] == '"') {
            pos++;
            // check if this is an escaped quote
            if (pos < input.size() && input[pos] == '"') {
                // escaped quote
                result += '"';
                continue;
            }
            return result;
        }
        result += input[pos];
    }
    throw InvalidInputException("Failed to parse quoted value - unterminated quote");
}

static vector<string> ParseQuotedList(const string &input, char list_separator) {
    vector<string> result;
    if (input.empty()) {
        return result;
    }
    idx_t pos = 0;
    while (true) {
        result.push_back(ParseQuotedValue(input, pos));
        if (pos >= input.size()) {
            break;
        }
        if (input[pos] != list_separator) {
            throw InvalidInputException("Failed to parse list - expected a %s", string(1, list_separator));
        }
        pos++;
    }
    return result;
}

struct DeltaColumnStats {
    explicit DeltaColumnStats() = default;

    string min;
    string max;
    idx_t null_count = 0;
    idx_t column_size_bytes = 0;
    bool contains_nan = false;
    bool has_null_count = false;
    bool has_min = false;
    bool has_max = false;
    bool any_valid = true;
    bool has_contains_nan = false;
};

static DeltaColumnStats ParseColumnStats(const vector<Value> col_stats) {
    DeltaColumnStats column_stats;
    for (idx_t stats_idx = 0; stats_idx < col_stats.size(); stats_idx++) {
        auto &stats_children = StructValue::GetChildren(col_stats[stats_idx]);
        auto &stats_name = StringValue::Get(stats_children[0]);
        auto &stats_value = StringValue::Get(stats_children[1]);
        if (stats_name == "min") {
            D_ASSERT(!column_stats.has_min);
            column_stats.min = stats_value;
            column_stats.has_min = true;
        } else if (stats_name == "max") {
            D_ASSERT(!column_stats.has_max);
            column_stats.max = stats_value;
            column_stats.has_max = true;
        } else if (stats_name == "null_count") {
            D_ASSERT(!column_stats.has_null_count);
            column_stats.has_null_count = true;
            column_stats.null_count = StringUtil::ToUnsigned(stats_value);
        } else if (stats_name == "column_size_bytes") {
            column_stats.column_size_bytes = StringUtil::ToUnsigned(stats_value);
        } else if (stats_name == "has_nan") {
            column_stats.has_contains_nan = true;
            column_stats.contains_nan = stats_value == "true";
        } else {
            throw NotImplementedException("Unsupported stats type \"%s\" in DuckLakeInsert::Sink()", stats_name);
        }
    }
    return column_stats;
}

static void AddWrittenFiles(DeltaInsertGlobalState &global_state, DataChunk &chunk) {
	for (idx_t r = 0; r < chunk.size(); r++) {
		DeltaDataFile data_file;
		data_file.file_name = chunk.GetValue(0, r).GetValue<string>();
		data_file.row_count = chunk.GetValue(1, r).GetValue<idx_t>();
		data_file.file_size_bytes = chunk.GetValue(2, r).GetValue<idx_t>();
		data_file.footer_size = chunk.GetValue(3, r).GetValue<idx_t>();
		// extract the column stats
		auto column_stats = chunk.GetValue(4, r);
		auto &map_children = MapValue::GetChildren(column_stats);

	    global_state.insert_count += data_file.row_count;

	    for (idx_t col_idx = 0; col_idx < map_children.size(); col_idx++) {
			auto &struct_children = StructValue::GetChildren(map_children[col_idx]);
			auto &col_name = StringValue::Get(struct_children[0]);
			auto &col_stats = MapValue::GetChildren(struct_children[1]);
			auto column_names = ParseQuotedList(col_name, '.');
			auto stats = ParseColumnStats(col_stats);

	        if (stats.has_null_count && stats.null_count > 0) {
	            auto constraint = global_state.not_null_constraints.find(column_names[0]);
	            if (constraint != global_state.not_null_constraints.end()) {
	                // We may have a not null constraint for this col, it's not nested so it
	                if (column_names.size() == 1) {
	                    throw ConstraintException("NOT NULL constraint failed: %s.%s", global_state.table_name, column_names[0]);
	                }

	                // Check paths
	                for (auto &constr : constraint->second) {
	                    if (col_name == constr.path) {
	                        throw ConstraintException("NOT NULL constraint failed: %s.%s", global_state.table_name, StringUtil::Join(column_names, "."));
	                    }
	                }
	            }
	        }
		}

	    // extract the partition info
		auto partition_info = chunk.GetValue(5, r);
		if (!partition_info.IsNull()) {
			auto &partition_children = MapValue::GetChildren(partition_info);
			for (idx_t col_idx = 0; col_idx < partition_children.size(); col_idx++) {
				auto &struct_children = StructValue::GetChildren(partition_children[col_idx]);
				auto &part_value = StringValue::Get(struct_children[1]);

				DeltaPartition file_partition_info;
				file_partition_info.partition_column_idx = col_idx;
				file_partition_info.partition_value = part_value;
				data_file.partition_values.push_back(std::move(file_partition_info));
			}
		}

		global_state.written_files.push_back(std::move(data_file));
	}
}

SinkResultType DeltaInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
    auto &global_state = input.global_state.Cast<DeltaInsertGlobalState>();

    if (chunk.size() != 1) {
        throw InternalException("DeltaInsert::Sink expects a single row containing output of the PhysicalCopy that should be its Source");
    }

    AddWrittenFiles(global_state, chunk);

    return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType DeltaInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
    auto &global_state = sink_state->Cast<DeltaInsertGlobalState>();
    auto value = Value::BIGINT(global_state.insert_count);
    chunk.SetCardinality(1);
    chunk.SetValue(0, 0, value);
    return SourceResultType::FINISHED;
}
//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType DeltaInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                       OperatorSinkFinalizeInput &input) const {
    auto &global_state = input.global_state.Cast<DeltaInsertGlobalState>();

    auto &transaction = DeltaTransaction::Get(context, table->catalog);
    vector<string> filenames;
    transaction.Append(context, global_state.written_files);

    return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//


//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string DeltaInsert::GetName() const {
	return table ? "DELTA_INSERT" : "DELTA_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> DeltaInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table ? table->name : info->Base().table;
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
static optional_ptr<CopyFunctionCatalogEntry> TryGetCopyFunction(DatabaseInstance &db, const string &name) {
    D_ASSERT(!name.empty());
    auto &system_catalog = Catalog::GetSystemCatalog(db);
    auto data = CatalogTransaction::GetSystemTransaction(db);
    auto &schema = system_catalog.GetSchema(data, DEFAULT_SCHEMA);
    return schema.GetEntry(data, CatalogType::COPY_FUNCTION_ENTRY, name)->Cast<CopyFunctionCatalogEntry>();
}

PhysicalOperator &DeltaCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
    optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for insertion into Delta table");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT clause not yet supported for insertion into Delta table");
	}

    // Lookup table here:
    optional_ptr<DeltaTableEntry> table_entry;
    if (child_catalog_mode) {
        // We need to fetch the table ourselves
        CatalogEntryRetriever retriever(context);
        EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, default_table);
        auto default_table_entry = LookupEntry(retriever, default_schema, lookup_info, OnEntryNotFound::THROW_EXCEPTION);
        table_entry = default_table_entry.entry->Cast<DeltaTableEntry>();
    } else {
        table_entry = op.table.Cast<DeltaTableEntry>();
    }

    string delta_path =  table_entry->snapshot->GetPaths()[0].path;

    // Create Copy Info
    auto info = make_uniq<CopyInfo>();
    info->file_path = delta_path;
    info->format = "parquet";
    info->is_from = false;

    // Get Parquet Copy function
    auto copy_fun = TryGetCopyFunction(*context.db, "parquet");
    if (!copy_fun) {
        throw MissingExtensionException("Did not find parquet copy function required to write to delta table");
    }


    auto partitions = table_entry->snapshot->GetPartitionColumns();
    vector<idx_t> partition_columns;
    if (!partitions.empty()) {
        auto column_names = table_entry->GetColumns().GetColumnNames();
        for (int64_t i = 0; i < partitions.size(); i++) {
            for (int64_t j = 0; j < column_names.size(); j++) {
                if (column_names[j] == partitions[i]) {
                    partition_columns.push_back(j);
                    break;
                }
            }
        }
    }

    // Bind Copy Function
    auto &columns = table_entry->GetColumns();
    CopyFunctionBindInput bind_input(*info);

    auto names_to_write = columns.GetColumnNames();
    auto types_to_write = columns.GetColumnTypes();

    auto function_data = copy_fun->function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

    auto &insert = planner.Make<DeltaInsert>(op, *table_entry, op.column_index_map);

    auto &physical_copy = planner.Make<PhysicalCopyToFile>(GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS), copy_fun->function, std::move(function_data), op.estimated_cardinality);
    auto &physical_copy_ref = physical_copy.Cast<PhysicalCopyToFile>();

    auto current_write_uuid = UUID::ToString(UUID::GenerateRandomUUID());

    physical_copy_ref.use_tmp_file = false;
    if (!partition_columns.empty()) {
        physical_copy_ref.filename_pattern.SetFilenamePattern("duckdb_" + current_write_uuid + "_{i}");
        physical_copy_ref.file_path = delta_path;
        physical_copy_ref.partition_output = true;
        physical_copy_ref.partition_columns = partition_columns;
        physical_copy_ref.write_empty_file = true;
    } else {
        physical_copy_ref.file_path = delta_path + "/duckdb-" + current_write_uuid + ".parquet";
        physical_copy_ref.partition_output = false;
        physical_copy_ref.write_empty_file = false;
    }

    physical_copy_ref.file_extension = "parquet";
    physical_copy_ref.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
    physical_copy_ref.per_thread_output = false;
    physical_copy_ref.rotate = false;
    physical_copy_ref.return_type = CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
    physical_copy_ref.write_partition_columns = true;
    physical_copy_ref.children.push_back(*plan);
    physical_copy_ref.names = names_to_write;
    physical_copy_ref.expected_types = types_to_write;
    physical_copy_ref.hive_file_pattern = true;

    insert.children.push_back(physical_copy);

	return insert;
}


} // namespace duckdb
