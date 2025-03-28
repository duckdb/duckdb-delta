#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp"
#include "functions/delta_scan/delta_scan.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

// Generate the correct Selection Vector Based on the Raw delta KernelBoolSlice dv and the row_id_column
// TODO: this probably is slower than needed (we can do with less branches in the for loop for most cases)
static SelectionVector DuckSVFromDeltaSV(const ffi::KernelBoolSlice &dv, Vector row_id_column, idx_t count,
                                         idx_t &select_count) {
	D_ASSERT(row_id_column.GetType() == LogicalType::BIGINT);

	UnifiedVectorFormat data;
	row_id_column.ToUnifiedFormat(count, data);
	auto row_ids = UnifiedVectorFormat::GetData<int64_t>(data);

	SelectionVector result {count};
	idx_t current_select = 0;
	for (idx_t i = 0; i < count; i++) {
		auto row_id = row_ids[data.sel->get_index(i)];

		if (row_id >= dv.len || dv.ptr[row_id]) {
			result.data()[current_select] = i;
			current_select++;
		}
	}

	select_count = current_select;

	return result;
}

// Note: this overrides MultifileReader::FinalizeBind removing the lines adding the hive_partitioning indexes
//       the reason is that we (ab)use those to use them to forward the delta partitioning information.
static void FinalizeBindBaseOverride(const MultiFileReaderOptions &file_options, const MultiFileReaderBindData &options,
                                     const string &filename,
                                     const vector<MultiFileReaderColumnDefinition> &local_columns,
                                     const vector<MultiFileReaderColumnDefinition> &global_columns,
                                     const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
                                     ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) {

	// create a map of name -> column index
	case_insensitive_map_t<idx_t> name_map;
	if (file_options.union_by_name) {
		for (idx_t col_idx = 0; col_idx < local_columns.size(); col_idx++) {
			auto &column = local_columns[col_idx];
			name_map[column.name] = col_idx;
		}
	}
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_idx = MultiFileGlobalIndex(i);
		auto &col_idx = global_column_ids[i];

		if (col_idx.IsRowIdColumn()) {
			// row-id
			reader_data.constant_map.Add(global_idx, Value::BIGINT(42));
			continue;
		}
		auto column_id = col_idx.GetPrimaryIndex();
		if (column_id == options.filename_idx) {
			// filename
			reader_data.constant_map.Add(global_idx, Value(filename));
			continue;
		}
		if (file_options.union_by_name) {
			auto &column = global_columns[column_id];
			auto &name = column.name;
			auto &type = column.type;

			auto entry = name_map.find(name);
			bool not_present_in_file = entry == name_map.end();
			if (not_present_in_file) {
				// we need to project a column with name \"global_name\" - but it does not exist in the current file
				// push a NULL value of the specified type
				reader_data.constant_map.Add(global_idx, Value(type));
				continue;
			}
		}
	}
}

// TODO: upstream this into MultiFileReaderColumnDefinition?
static vector<MultiFileReaderColumnDefinition> ColumnsFromNamesAndTypes(const vector<string> &names,
                                                                        const vector<LogicalType> &types) {
	vector<MultiFileReaderColumnDefinition> columns;
	D_ASSERT(names.size() == types.size());
	for (idx_t i = 0; i < names.size(); i++) {
		auto &name = names[i];
		auto &type = types[i];
		columns.emplace_back(name, type);

		if (type.IsNested()) {
			if (type.InternalType() == PhysicalType::STRUCT) {
				auto &struct_children = StructType::GetChildTypes(type);
				vector<string> child_names;
				vector<LogicalType> child_types;
				for (const auto &child : struct_children) {
					child_names.push_back(child.first);
					child_types.push_back(child.second);
				}
				columns.back().children = ColumnsFromNamesAndTypes(child_names, child_types);
			} else if (type.InternalType() == PhysicalType::LIST) {
				auto &list_child = ListType::GetChildType(type);
				columns.back().children = ColumnsFromNamesAndTypes({""}, {list_child});
			} else if (type.InternalType() == PhysicalType::ARRAY) {
				auto &list_child = ArrayType::GetChildType(type);
				columns.back().children = ColumnsFromNamesAndTypes({""}, {list_child});
			} else {
				throw InternalException("Unknown nested type found in ColumnsFromNamesAndTypes: '%s'", type.ToString());
			}
		}
	}

	return columns;
}

// Parses the columns that are used by the delta extension into
void DeltaMultiFileReaderGlobalState::SetColumnIdx(const string &column, idx_t idx) {
	if (column == "file_row_number") {
		file_row_number_idx = idx;
		return;
	} else if (column == "delta_file_number") {
		delta_file_number_idx = idx;
		return;
	}
	throw IOException("Unknown column '%s' found as required by the DeltaMultisFileReader");
}

bool DeltaMultiFileReader::Bind(MultiFileReaderOptions &options, MultiFileList &files,
                                vector<LogicalType> &return_types, vector<string> &names,
                                MultiFileReaderBindData &bind_data) {
	auto &delta_snapshot = dynamic_cast<DeltaMultiFileList &>(files);

	delta_snapshot.Bind(return_types, names);

	// We need to parse this option
	bool file_row_number_enabled = options.custom_options.find("file_row_number") != options.custom_options.end();
	if (file_row_number_enabled) {
		bind_data.file_row_number_idx = names.size();
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("file_row_number");
	} else {
		// TODO: this is a bogus ID? Change for flag indicating it should be enabled?
		bind_data.file_row_number_idx = names.size();
	}

	return true;
};

void DeltaMultiFileReader::BindOptions(MultiFileReaderOptions &options, MultiFileList &files,
                                       vector<LogicalType> &return_types, vector<string> &names,
                                       MultiFileReaderBindData &bind_data) {

	// Disable all other multifilereader options
	options.auto_detect_hive_partitioning = false;
	options.hive_partitioning = false;
	options.union_by_name = false;

	MultiFileReader::BindOptions(options, files, return_types, names, bind_data);

	// We abuse the hive_partitioning_indexes to forward partitioning information to DuckDB
	// TODO: we should clean up this API: hive_partitioning_indexes is confusingly named here. We should make this
	// generic
	auto pushdown_partition_info_setting = options.custom_options.find("pushdown_partition_info");
	if (pushdown_partition_info_setting == options.custom_options.end() ||
	    pushdown_partition_info_setting->second.GetValue<bool>()) {
		auto &snapshot = dynamic_cast<DeltaMultiFileList &>(files);
		auto partitions = snapshot.GetPartitionColumns();
		for (auto &part : partitions) {
			idx_t hive_partitioning_index;
			auto lookup = std::find_if(names.begin(), names.end(),
			                           [&](const string &col_name) { return StringUtil::CIEquals(col_name, part); });
			if (lookup != names.end()) {
				// hive partitioning column also exists in file - override
				auto idx = NumericCast<idx_t>(lookup - names.begin());
				hive_partitioning_index = idx;
			} else {
				throw IOException("Delta Snapshot returned partition column that is not present in the schema");
			}
			bind_data.hive_partitioning_indexes.emplace_back(part, hive_partitioning_index);
		}
	}

	auto demo_gen_col_opt = options.custom_options.find("delta_file_number");
	if (demo_gen_col_opt != options.custom_options.end()) {
		if (demo_gen_col_opt->second.GetValue<bool>()) {
			names.push_back("delta_file_number");
			return_types.push_back(LogicalType::UBIGINT);
		}
	}

	// FIXME: this is slightly hacky here
	bind_data.schema = ColumnsFromNamesAndTypes(names, return_types);
}

void DeltaMultiFileReader::FinalizeBind(const MultiFileReaderOptions &file_options,
                                        const MultiFileReaderBindData &options, const string &filename,
                                        const vector<MultiFileReaderColumnDefinition> &local_columns,
                                        const vector<MultiFileReaderColumnDefinition> &global_columns,
                                        const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
                                        ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) {
	FinalizeBindBaseOverride(file_options, options, filename, local_columns, global_columns, global_column_ids,
	                         reader_data, context, global_state);

	// Handle custom delta option set in MultiFileReaderOptions::custom_options
	auto file_number_opt = file_options.custom_options.find("delta_file_number");
	if (file_number_opt != file_options.custom_options.end()) {
		if (file_number_opt->second.GetValue<bool>()) {
			D_ASSERT(global_state);
			auto &delta_global_state = global_state->Cast<DeltaMultiFileReaderGlobalState>();
			D_ASSERT(delta_global_state.delta_file_number_idx != DConstants::INVALID_INDEX);

			// We add the constant column for the delta_file_number option
			// NOTE: we add a placeholder here, to demonstrate how we can also populate extra columns in the
			// FinalizeChunk
			auto global_idx = MultiFileGlobalIndex(delta_global_state.delta_file_number_idx);
			reader_data.constant_map.Add(global_idx, Value::UBIGINT(0));
		}
	}

	// Get the metadata for this file
	D_ASSERT(global_state->file_list);
	const auto &snapshot = dynamic_cast<const DeltaMultiFileList &>(*global_state->file_list);
	auto &file_metadata = snapshot.GetMetaData(reader_data.file_list_idx.GetIndex());

	if (!file_metadata.partition_map.empty()) {
		for (idx_t i = 0; i < global_column_ids.size(); i++) {
			auto global_idx = MultiFileGlobalIndex(i);
			column_t col_id = global_column_ids[i].GetPrimaryIndex();

			if (IsRowIdColumnId(col_id)) {
				continue;
			}
			auto col_partition_entry = file_metadata.partition_map.find(global_columns[col_id].name);
			if (col_partition_entry != file_metadata.partition_map.end()) {
				auto &current_type = global_columns[col_id].type;
				if (current_type == LogicalType::BLOB) {
					reader_data.constant_map.Add(global_idx, col_partition_entry->second);
				} else {
					auto maybe_value = Value(col_partition_entry->second).DefaultCastAs(current_type);
					reader_data.constant_map.Add(global_idx, maybe_value);
				}
			}
		}
	}
}

shared_ptr<MultiFileList> DeltaMultiFileReader::CreateFileList(ClientContext &context, const vector<string> &paths,
                                                               FileGlobOptions options) {
	if (paths.size() != 1) {
		throw BinderException("'delta_scan' only supports single path as input");
	}

	if (snapshot) {
		// TODO: assert that we are querying the same path as this injected snapshot
		// This takes the kernel snapshot from the delta snapshot and ensures we use that snapshot for reading
		if (snapshot) {
			return snapshot;
		}
	}

	return make_shared_ptr<DeltaMultiFileList>(context, paths[0]);
}

unique_ptr<MultiFileReaderGlobalState>
DeltaMultiFileReader::InitializeGlobalState(ClientContext &context, const MultiFileReaderOptions &file_options,
                                            const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
                                            const vector<MultiFileReaderColumnDefinition> &global_columns,
                                            const vector<ColumnIndex> &global_column_ids) {
	vector<LogicalType> extra_columns;
	vector<pair<string, idx_t>> mapped_columns;

	// Create a map of the columns that are in the projection
	case_insensitive_map_t<idx_t> selected_columns;
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_id = global_column_ids[i].GetPrimaryIndex();
		if (IsRowIdColumnId(global_id)) {
			continue;
		}

		auto &global_name = global_columns[global_id].name;
		selected_columns.insert({global_name, i});
	}

	// TODO: only add file_row_number column if there are deletes
	case_insensitive_map_t<LogicalType> columns_to_map = {
	    {"file_row_number", LogicalType::BIGINT},
	};

	// Add the delta_file_number column to the columns to map
	auto demo_gen_col_opt = file_options.custom_options.find("delta_file_number");
	if (demo_gen_col_opt != file_options.custom_options.end()) {
		if (demo_gen_col_opt->second.GetValue<bool>()) {
			columns_to_map.insert({"delta_file_number", LogicalType::UBIGINT});
		}
	}

	// Map every column to either a column in the projection, or add it to the extra columns if it doesn't exist
	idx_t col_offset = 0;
	for (const auto &required_column : columns_to_map) {
		// First check if the column is in the projection
		auto res = selected_columns.find(required_column.first);
		if (res != selected_columns.end()) {
			// The column is in the projection, no special handling is required; we simply store the index
			mapped_columns.push_back({required_column.first, res->second});
			continue;
		}

		// The column is NOT in the projection: it needs to be added as an extra_column

		// Calculate the index of the added column (extra columns are added after all other columns)
		idx_t current_col_idx = global_column_ids.size() + col_offset++;

		// Add column to the map, to ensure the MultiFileReader can find it when processing the Chunk
		mapped_columns.push_back({required_column.first, current_col_idx});

		// Ensure the result DataChunk has a vector of the correct type to store this column
		extra_columns.push_back(required_column.second);
	}

	auto res = make_uniq<DeltaMultiFileReaderGlobalState>(extra_columns, &file_list);

	// Parse all the mapped columns into the DeltaMultiFileReaderGlobalState for easy use;
	for (const auto &mapped_column : mapped_columns) {
		res->SetColumnIdx(mapped_column.first, mapped_column.second);
	}

	return std::move(res);
}

// Returns a map of every local field name to a global field name (both columns and struct fields)
static void ParseNameMaps(vector<unique_ptr<ParsedExpression>> &transform_expression,
                          const vector<MultiFileReaderColumnDefinition> &global_columns,
                          unordered_map<string, string> &global_to_local) {
	auto &column_expressions = KernelUtils::UnpackTopLevelStruct(transform_expression);

	D_ASSERT(column_expressions.size() <= global_columns.size()); // tODO throw

	for (idx_t i = 0; i < column_expressions.size(); i++) {
		auto &expression = column_expressions[i];
		// auto &column_definition = global_columns[i];

		if (expression->type == ExpressionType::FUNCTION) {
			if (expression->Cast<FunctionExpression>().function_name != "struct_pack") {
				throw IOException("Unexpected function of root expression returned by delta kernel: %s",
				                  expression->Cast<FunctionExpression>().function_name);
			}
			// FIXME: Currently we don't traverse into nested types, since the kernel transforms don't contain them yet

			// auto &expression_children = expression->Cast<FunctionExpression>().children;
			// ParseNameMaps(expression_children, column_definition.children, local_to_global, global_to_local);

		} else if (expression->type == ExpressionType::COLUMN_REF) {
			auto local_name = expression->Cast<ColumnRefExpression>().GetColumnName();
			auto global_name = global_columns[i].name;

			global_to_local[global_name] = local_name;
		}
	}
}

static void DetectUnsupportedTypeCast(const LogicalType &local_type, const LogicalType &global_type) {
	if (local_type.IsNested() && local_type != global_type) {
		throw NotImplementedException("Unsupported type cast detected in Delta table '%s' -> '%s'. DuckDB currently "
		                              "does not support column mapping for nested types.",
		                              local_type.ToString(), global_type.ToString());
	}
}

// This code is duplicated from MultiFileReader::CreateNameMapping the difference is that for columns that are not found
// in the parquet files, we just add null constant columns
static void CustomMulfiFileNameMapping(const string &file_name,
                                       const vector<MultiFileReaderColumnDefinition> &local_columns,
                                       const vector<MultiFileReaderColumnDefinition> &global_columns,
                                       const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
                                       const string &initial_file,
                                       optional_ptr<MultiFileReaderGlobalState> global_state) {
	// we have expected types: create a map of name -> column index
	case_insensitive_map_t<MultiFileLocalColumnId> local_name_map;
	for (idx_t col_id = 0; col_id < local_columns.size(); col_id++) {
		local_name_map.emplace(local_columns[col_id].name, MultiFileLocalColumnId(col_id));
	}

	auto delta_list = dynamic_cast<const DeltaMultiFileList *>(global_state->file_list.get());

	auto &metadata = delta_list->GetMetaData(reader_data.file_list_idx.GetIndex());

	unordered_map<string, string> global_to_local;
	if (metadata.transform_expression) {
		ParseNameMaps(*metadata.transform_expression, global_columns, global_to_local);
	}

	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_index = MultiFileGlobalIndex(i);
		// check if this is a constant column
		bool constant = false;
		for (auto &entry : reader_data.constant_map) {
			if (entry.column_idx.GetIndex() == i) {
				constant = true;
				break;
			}
		}
		if (constant) {
			// this column is constant for this file
			continue;
		}
		// not constant - look up the column in the name map
		auto global_id = global_column_ids[i].GetPrimaryIndex();
		if (global_id >= global_columns.size()) {
			throw InternalException(
			    "MultiFileReader::CreatePositionalMapping - global_id is out of range in global_types for this file");
		}
		auto &global_name = global_columns[global_id].name;

		string local_name;
		if (metadata.transform_expression) {
			auto local_name_lookup = global_to_local.find(global_name);
			if (local_name_lookup == global_to_local.end()) {
				if (global_name == "file_row_number") {
					// Special case file_row_number column, we
					local_name = global_name;
				} else {
					throw IOException(
					    "Column '%s' from the schema was not found in the transformation expression returned by kernel",
					    global_name);
				}
			} else {
				local_name = local_name_lookup->second;
			}
		} else {
			local_name = global_name;
		}

		auto entry = local_name_map.find(local_name);
		if (entry == local_name_map.end()) {
			// FIXME: this override is pretty hacky: for missing columns we just insert NULL constants
			auto &global_type = global_columns[global_id].type;
			Value val(global_type);
			reader_data.constant_map.Add(global_index, val);
			continue;
		}
		// we found the column in the local file - check if the types are the same
		auto local_id = MultiFileLocalColumnId(entry->second);
		D_ASSERT(global_id < global_columns.size());
		D_ASSERT(local_id < local_columns.size());
		auto &global_type = global_columns[global_id].type;
		auto local_type = local_columns[local_id].type;

		DetectUnsupportedTypeCast(local_type, global_type);

		if (global_type != local_type) {
			reader_data.cast_map[local_id] = global_type;
		}
		// the types are the same - create the mapping
		reader_data.column_mapping.push_back(global_index);
		reader_data.column_ids.push_back(local_id);
	}

	reader_data.empty_columns = reader_data.column_ids.empty();
}

void DeltaMultiFileReader::CreateColumnMapping(const string &file_name,
                                               const vector<MultiFileReaderColumnDefinition> &local_columns,
                                               const vector<MultiFileReaderColumnDefinition> &global_columns,
                                               const vector<ColumnIndex> &global_column_ids,
                                               MultiFileReaderData &reader_data,
                                               const MultiFileReaderBindData &bind_data, const string &initial_file,
                                               optional_ptr<MultiFileReaderGlobalState> global_state) {
	// First call the base implementation to do most mapping
	CustomMulfiFileNameMapping(file_name, local_columns, global_columns, global_column_ids, reader_data, initial_file,
	                           global_state);

	// Then we handle delta specific mapping
	D_ASSERT(global_state);
	auto &delta_global_state = global_state->Cast<DeltaMultiFileReaderGlobalState>();

	// Check if the file_row_number column is an "extra_column" which is not part of the projection
	if (delta_global_state.file_row_number_idx >= global_column_ids.size()) {
		D_ASSERT(delta_global_state.file_row_number_idx != DConstants::INVALID_INDEX);

		// Build the name map
		case_insensitive_map_t<MultiFileLocalColumnId> name_map;
		for (idx_t col_id = 0; col_id < local_columns.size(); col_id++) {
			name_map.emplace(local_columns[col_id].name, MultiFileLocalColumnId(col_id));
		}

		// Lookup the required column in the local map
		auto entry = name_map.find("file_row_number");
		if (entry == name_map.end()) {
			throw IOException("Failed to find the file_row_number column");
		}

		// Register the column to be scanned from this file
		reader_data.column_ids.push_back(entry->second);
		auto global_idx = MultiFileGlobalIndex(delta_global_state.file_row_number_idx);
		reader_data.column_mapping.push_back(global_idx);
	}

	// This may have changed: update it
	reader_data.empty_columns = reader_data.column_ids.empty();
}

void DeltaMultiFileReader::FinalizeChunk(ClientContext &context, const MultiFileReaderBindData &bind_data,
                                         const MultiFileReaderData &reader_data, DataChunk &chunk,
                                         optional_ptr<MultiFileReaderGlobalState> global_state) {
	// Base class finalization first
	MultiFileReader::FinalizeChunk(context, bind_data, reader_data, chunk, global_state);

	D_ASSERT(global_state);
	auto &delta_global_state = global_state->Cast<DeltaMultiFileReaderGlobalState>();
	D_ASSERT(delta_global_state.file_list);

	// Get the metadata for this file
	const auto &snapshot = dynamic_cast<const DeltaMultiFileList &>(*global_state->file_list);
	auto &metadata = snapshot.GetMetaData(reader_data.file_list_idx.GetIndex());

	if (metadata.selection_vector.ptr && chunk.size() != 0) {
		D_ASSERT(delta_global_state.file_row_number_idx != DConstants::INVALID_INDEX);
		auto &file_row_number_column = chunk.data[delta_global_state.file_row_number_idx];

		// Construct the selection vector using the file_row_number column and the raw selection vector from delta
		idx_t select_count;
		auto sv = DuckSVFromDeltaSV(metadata.selection_vector, file_row_number_column, chunk.size(), select_count);
		chunk.Slice(sv, select_count);
	}

	// Note: this demo function shows how we can use DuckDB's Binder create expression-based generated columns
	if (delta_global_state.delta_file_number_idx != DConstants::INVALID_INDEX) {
		//! Create Dummy expression (0 + file_number)
		vector<unique_ptr<ParsedExpression>> child_expr;
		child_expr.push_back(make_uniq<ConstantExpression>(Value::UBIGINT(0)));
		child_expr.push_back(make_uniq<ConstantExpression>(Value::UBIGINT(7)));
		unique_ptr<ParsedExpression> expr =
		    make_uniq<FunctionExpression>("+", std::move(child_expr), nullptr, nullptr, false, true);

		//! s dummy expression
		auto binder = Binder::CreateBinder(context);
		ExpressionBinder expr_binder(*binder, context);
		auto bound_expr = expr_binder.Bind(expr, nullptr);

		//! Execute dummy expression into result column
		ExpressionExecutor expr_executor(context);
		expr_executor.AddExpression(*bound_expr);

		//! Execute the expression directly into the output Chunk
		expr_executor.ExecuteExpression(chunk.data[delta_global_state.delta_file_number_idx]);
	}
};

bool DeltaMultiFileReader::ParseOption(const string &key, const Value &val, MultiFileReaderOptions &options,
                                       ClientContext &context) {
	auto loption = StringUtil::Lower(key);

	if (loption == "delta_file_number") {
		options.custom_options[loption] = val;
		return true;
	}

	// We need to capture this one to know whether to emit
	if (loption == "file_row_number") {
		options.custom_options[loption] = val;
		return true;
	}

	if (loption == "pushdown_partition_info") {
		options.custom_options["pushdown_partition_info"] = val;
		return true;
	}

	// We need to capture this one to know whether to emit
	if (loption == "pushdown_filters") {
		options.custom_options["pushdown_filters"] = val;
		return true;
	}

	return MultiFileReader::ParseOption(key, val, options, context);
}

} // namespace duckdb