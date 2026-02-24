#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp"
#include "functions/delta_scan/delta_scan.hpp"

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

constexpr column_t DeltaMultiFileReader::DELTA_FILE_NUMBER_COLUMN_ID;

struct DeltaDeleteFilter : public DeleteFilter {
public:
	DeltaDeleteFilter(const ffi::KernelBoolSlice &dv) : dv(dv) {
	}

public:
	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override {
		if (count == 0) {
			return 0;
		}
		result_sel.Initialize(STANDARD_VECTOR_SIZE);
		idx_t current_select = 0;
		for (idx_t i = 0; i < count; i++) {
			auto row_id = i + start_row_index;

			const bool is_selected = row_id >= dv.len || dv.ptr[row_id];
			result_sel.set_index(current_select, i);
			current_select += is_selected;
		}
		return current_select;
	}

public:
	const ffi::KernelBoolSlice &dv;
};

void FinalizeBindBaseOverride(MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
                              const MultiFileReaderBindData &options,
                              const vector<MultiFileColumnDefinition> &global_columns,
                              const vector<ColumnIndex> &global_column_ids, ClientContext &context,
                              optional_ptr<MultiFileReaderGlobalState> global_state) {
	// create a map of name -> column index
	auto &local_columns = reader_data.reader->GetColumns();
	auto &filename = reader_data.reader->GetFileName();
	case_insensitive_map_t<idx_t> name_map;
	if (file_options.union_by_name) {
		for (idx_t col_idx = 0; col_idx < local_columns.size(); col_idx++) {
			auto &column = local_columns[col_idx];
			name_map[column.name] = col_idx;
		}
	}
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_idx = MultiFileGlobalIndex(i);
		auto &col_id = global_column_ids[i];
		auto column_id = col_id.GetPrimaryIndex();
		if ((options.filename_idx.IsValid() && column_id == options.filename_idx.GetIndex()) ||
		    column_id == MultiFileReader::COLUMN_IDENTIFIER_FILENAME) {
			// filename
			reader_data.constant_map.Add(global_idx, Value(filename));
			continue;
		}
		if (column_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_INDEX) {
			// filename
			reader_data.constant_map.Add(global_idx, Value::UBIGINT(reader_data.reader->file_list_idx.GetIndex()));
			continue;
		}
		if (column_id == DeltaMultiFileReader::DELTA_FILE_NUMBER_COLUMN_ID) {
			// filename
			reader_data.constant_map.Add(global_idx, Value::UBIGINT(7));
			continue;
		}

		if (IsVirtualColumn(column_id)) {
			continue;
		}
		if (file_options.union_by_name) {
			auto &column = global_columns[column_id];
			auto name = column.name;
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

bool DeltaMultiFileReader::Bind(MultiFileOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
                                vector<string> &names, MultiFileReaderBindData &bind_data) {
	auto &delta_snapshot = dynamic_cast<DeltaMultiFileList &>(files);

	auto log_tail_setting = options.custom_options.find("log_tail");
	if (log_tail_setting != options.custom_options.end()) {
		delta_snapshot.delta_log_path = make_uniq<DeltaLogPathArray>(log_tail_setting->second);
	}

	delta_snapshot.Bind(return_types, names);

	return true;
}

void DeltaMultiFileReader::BindOptions(MultiFileOptions &options, MultiFileList &files,
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

	// FIXME: this is slightly hacky here
	bind_data.schema = DeltaMultiFileColumnDefinition::ColumnsFromNamesAndTypes(names, return_types);

	// Set defaults
	for (auto &col : bind_data.schema) {
		col.default_expression = make_uniq<ConstantExpression>(Value(col.type));
	}
}

ReaderInitializeType DeltaMultiFileReader::InitializeReader(MultiFileReaderData &reader_data,
                                                            const MultiFileBindData &bind_data,
                                                            const vector<MultiFileColumnDefinition> &global_columns,
                                                            const vector<ColumnIndex> &global_column_ids,
                                                            optional_ptr<TableFilterSet> table_filters,
                                                            ClientContext &context, MultiFileGlobalState &gstate) {
	auto &global_state = gstate.multi_file_reader_state;
	D_ASSERT(global_state);
	auto &delta_global_state = global_state->Cast<DeltaMultiFileReaderGlobalState>();
	auto &snapshot = delta_global_state.file_list->Cast<DeltaMultiFileList>();

	auto &scan_columns = snapshot.GetLazyLoadedGlobalColumns();

	// We need to override the global columns, because only now we have the correct column mapping information
	vector<MultiFileColumnDefinition> overridden_global_columns =
	    DeltaMultiFileColumnDefinition::ConvertToBase(scan_columns);
	if (scan_columns.size() != global_columns.size()) {
		overridden_global_columns = DeltaMultiFileColumnDefinition::ConvertToBase(scan_columns);
		for (idx_t i = scan_columns.size(); i < global_columns.size(); i++) {
			overridden_global_columns.push_back(global_columns[i]);
		}
	} else {
		overridden_global_columns = DeltaMultiFileColumnDefinition::ConvertToBase(scan_columns);
	}

	FinalizeBind(reader_data, bind_data.file_options, bind_data.reader_bind, overridden_global_columns,
	             global_column_ids, context, global_state);
	return CreateMapping(context, reader_data, overridden_global_columns, global_column_ids, table_filters,
	                     gstate.file_list, bind_data.reader_bind, bind_data.virtual_columns);
}

void DeltaMultiFileReader::FinalizeBind(MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
                                        const MultiFileReaderBindData &options,
                                        const vector<MultiFileColumnDefinition> &global_columns,
                                        const vector<ColumnIndex> &global_column_ids, ClientContext &context,
                                        optional_ptr<MultiFileReaderGlobalState> global_state) {
	FinalizeBindBaseOverride(reader_data, file_options, options, global_columns, global_column_ids, context,
	                         global_state);

	// Get the metadata for this file
	D_ASSERT(global_state->file_list);
	const auto &snapshot = dynamic_cast<const DeltaMultiFileList &>(*global_state->file_list);
	auto &file_metadata = snapshot.GetMetaData(reader_data.reader->file_list_idx.GetIndex());

	// TODO: inject these in the global column definitions instead?
	if (!file_metadata.partition_map.empty()) {
		for (idx_t i = 0; i < global_column_ids.size(); i++) {
			auto global_idx = MultiFileGlobalIndex(i);
			column_t col_id = global_column_ids[i].GetPrimaryIndex();

			if (IsVirtualColumn(col_id)) {
				continue;
			}

			auto col_partition_entry = file_metadata.partition_map.find(global_columns[col_id].name);
			if (col_partition_entry != file_metadata.partition_map.end()) {
				auto &current_type = global_columns[col_id].type;
				auto maybe_value = Value(col_partition_entry->second).DefaultCastAs(current_type);
				reader_data.constant_map.Add(global_idx, maybe_value);
			}
		}
	}

	auto &reader = *reader_data.reader;
	if (file_metadata.selection_vector.ptr) {
		//! Push the deletes into the parquet scan
		reader.deletion_filter = make_uniq<DeltaDeleteFilter>(file_metadata.selection_vector);
	}
}

shared_ptr<MultiFileList> DeltaMultiFileReader::CreateFileList(ClientContext &context, const vector<string> &paths,
                                                               const FileGlobInput &glob_input) {
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

	return make_shared_ptr<DeltaMultiFileList>(context, paths[0], DConstants::INVALID_INDEX);
}

unique_ptr<MultiFileReaderGlobalState>
DeltaMultiFileReader::InitializeGlobalState(ClientContext &context, const MultiFileOptions &file_options,
                                            const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
                                            const vector<MultiFileColumnDefinition> &global_columns,
                                            const vector<ColumnIndex> &global_column_ids) {
	vector<LogicalType> extra_columns;
	vector<pair<string, idx_t>> mapped_columns;

	// Create a map of the columns that are in the projection
	case_insensitive_map_t<idx_t> selected_columns;
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_id = global_column_ids[i].GetPrimaryIndex();

		if (IsVirtualColumn(global_id)) {
			continue;
		}

		auto global_name = global_columns[global_id].name;
		selected_columns.insert({global_name, i});
	}

	auto res = make_uniq<DeltaMultiFileReaderGlobalState>(extra_columns, &file_list);

	return std::move(res);
}

void DeltaMultiFileReader::FinalizeChunk(ClientContext &context, const MultiFileBindData &bind_data,
                                         BaseFileReader &reader, const MultiFileReaderData &reader_data,
                                         DataChunk &input_chunk, DataChunk &output_chunk, ExpressionExecutor &executor,
                                         optional_ptr<MultiFileReaderGlobalState> global_state) {
	// Base class finalization first
	MultiFileReader::FinalizeChunk(context, bind_data, reader, reader_data, input_chunk, output_chunk, executor,
	                               global_state);

	D_ASSERT(global_state);
	auto &delta_global_state = global_state->Cast<DeltaMultiFileReaderGlobalState>();
	D_ASSERT(delta_global_state.file_list);
};

bool DeltaMultiFileReader::ParseOption(const string &key, const Value &val, MultiFileOptions &options,
                                       ClientContext &context) {
	auto loption = StringUtil::Lower(key);

	if (loption == "pushdown_partition_info") {
		options.custom_options["pushdown_partition_info"] = val;
		return true;
	}

	if (loption == "log_tail") {
		options.custom_options["log_tail"] = val;
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
