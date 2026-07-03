#include "delta_functions.hpp"
#include "functions/delta_scan/delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp"
#include "functions/delta_scan/physical_delta_scan.hpp"

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "functions/delta_scan/sm_sdk.hpp"

namespace duckdb {

DeltaFilterPushdownMode DeltaEnumUtils::FromString(const string &str) {
	auto str_to_lower = StringUtil::Lower(str);
	if (str_to_lower == "none") {
		return DeltaFilterPushdownMode::NONE;
	}
	if (str_to_lower == "all") {
		return DeltaFilterPushdownMode::ALL;
	}
	if (str_to_lower == "constant_only") {
		return DeltaFilterPushdownMode::CONSTANT_ONLY;
	}
	if (str_to_lower == "dynamic_only") {
		return DeltaFilterPushdownMode::DYNAMIC_ONLY;
	}
	throw InvalidInputException("Unknown Filter pushdown mode: %s", str);
}

string DeltaEnumUtils::ToString(const DeltaFilterPushdownMode &mode) {
	switch (mode) {
	case DeltaFilterPushdownMode::NONE:
		return "none";
	case DeltaFilterPushdownMode::ALL:
		return "all";
	case DeltaFilterPushdownMode::DYNAMIC_ONLY:
		return "dynamic_only";
	case DeltaFilterPushdownMode::CONSTANT_ONLY:
		return "constant_only";
	default:
		throw InvalidInputException("Unknown delta pushdown mode: %s", mode);
	}
}

static InsertionOrderPreservingMap<string> DeltaFunctionToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;

	if (input.table_function.function_info) {
		auto &table_info = input.table_function.function_info->Cast<DeltaFunctionInfo>();
		result["Table"] = table_info.table_name;
	}

	return result;
}

virtual_column_map_t DeltaVirtualColumns(ClientContext &, optional_ptr<FunctionData> bind_data_p) {
	virtual_column_map_t result;
	result.insert(
	    make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILENAME, TableColumn("filename", LogicalType::VARCHAR)));
	result.insert(make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
	                        TableColumn("file_row_number", LogicalType::BIGINT)));
	result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::BIGINT)));
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));

	result.insert(make_pair(DeltaMultiFileReader::DELTA_FILE_NUMBER_COLUMN_ID,
	                        TableColumn("delta_file_number", LogicalType::UBIGINT)));

	auto &bind_data = bind_data_p->Cast<MultiFileBindData>();
	bind_data.virtual_columns = result;
	return result;
}

static void DeltaScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                               const TableFunction &function) {
	throw NotImplementedException("DeltaScan serialization not implemented");
}

static unique_ptr<FunctionData> DeltaScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	throw NotImplementedException("DeltaScan deserialization not implemented");
}

// Plan-based scan (design C′): route delta_scan through a custom LogicalDeltaGet ->
// PhysicalDeltaScan instead of the default LogicalGet -> PhysicalTableScan, so we own the data-stage
// scan operator (and, in later milestones, its pipeline structure). Gated by DELTA_KERNEL_PLAN_OP;
// returning nullptr falls back to the standard table-function bind + PhysicalTableScan path.
static unique_ptr<LogicalOperator> DeltaScanBindOperator(ClientContext &context, TableFunctionBindInput &input,
                                                         idx_t bind_index, vector<string> &return_names) {
	// DELTA_KERNEL_PLAN_OP routes delta_scan through the custom operator. DELTA_KERNEL_PLAN_SM_ASYNC
	// (Option B / Tier-2 async metadata-scan SM) is driven from inside PhysicalDeltaLoad's source, so it
	// also needs the operator route — enable it implicitly. DELTA_KERNEL_PLAN_SM (the synchronous
	// metadata-scan path) also routes through the operator so its reconciliation can be attached as a
	// VISIBLE child subplan at optimize time (see DeltaScanPushdownIntoOperator) — the WHERE predicate
	// is only known post-pushdown, so the subplan is built there, not here.
	if (!std::getenv("DELTA_KERNEL_PLAN_OP") && !std::getenv("DELTA_KERNEL_PLAN_SM_ASYNC") &&
	    !std::getenv("DELTA_KERNEL_PLAN_SM")) {
		return nullptr;
	}
	auto &table_function = input.table_function;
	if (!table_function.bind) {
		return nullptr;
	}
	// Run the standard (multi-file) bind ourselves to produce the bound schema + DeltaMultiFileList,
	// then wrap it in the custom logical operator.
	vector<LogicalType> return_types;
	auto bind_data = table_function.bind(context, input, return_types, return_names);
	// Async metadata-scan SM mode: mark the file list streaming NOW (at bind) so any bind/plan-time
	// GetCardinality / GetTotalFileCount call serves the (empty) resolved_files directly instead of
	// triggering the synchronous kernel scan — leaving the WHOLE reconciliation to the execution-time
	// async driver in PhysicalDeltaLoad. Without this, cardinality estimation would populate the list at
	// bind time (defeating Option B and double-populating against the driver).
	// Both async SM and synchronous-SM-as-visible-subplan feed the file list from PhysicalDeltaLoad's
	// streaming sink instead of a bind/plan-time synchronous kernel scan. Mark it streaming NOW so any
	// bind/plan-time GetCardinality / GetTotalFileCount serves the (empty) resolved_files directly.
	if (std::getenv("DELTA_KERNEL_PLAN_SM_ASYNC") || std::getenv("DELTA_KERNEL_PLAN_SM")) {
		auto &mf_bind_data = bind_data->Cast<MultiFileBindData>();
		mf_bind_data.file_list->Cast<DeltaMultiFileList>().MarkStreamingPopulated();
	}
	virtual_column_map_t virtual_columns;
	if (table_function.get_virtual_columns) {
		virtual_columns = table_function.get_virtual_columns(context, bind_data.get());
	}
	auto op = make_uniq<LogicalDeltaGet>(bind_index, table_function, std::move(bind_data), std::move(return_types),
	                                     return_names, std::move(virtual_columns), input.inputs, input.named_parameters);

	return std::move(op);
}

// delta_scan as one whole-plan SQL string (design P2): instead of attaching the metadata plan as a
// build child (DELTA_KERNEL_PLAN_BUILD), lower the ENTIRE kernel ResultPlan to SQL (the data Load is
// the delta_load TVF) and hand it back as a subquery the binder re-binds. Gated by
// DELTA_KERNEL_PLAN_SQL_TVF; returns nullptr to fall through to the standard/bind_operator paths.
static unique_ptr<TableRef> DeltaScanBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (!std::getenv("DELTA_KERNEL_PLAN_SQL_TVF")) {
		return nullptr;
	}
	if (input.inputs.empty()) {
		return nullptr;
	}
	string table_path = input.inputs[0].GetValue<string>();
	int64_t scan_version = -1;
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(kv.first) == "version" && !kv.second.IsNull()) {
			auto v = kv.second.GetValue<int64_t>();
			scan_version = (v < 0) ? -1 : v;
		}
	}
	// DuckDB drives the kernel scan state machine (get_step -> run the reduce SQL in DuckDB ->
	// submit), then lowers the terminal ResultPlan to the data-stage SQL.
	string sql = delta_sdk::DriveScan(table_path, scan_version, /* metadata_only= */ false, context);

	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw IOException("delta_scan bind_replace: expected a single SELECT statement from plan lowering");
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

// delta_scan_metadata(path, [version]): runs ONLY the scan-metadata phase — DuckDB drives the
// kernel's metadata-only scan SM and returns the surviving-file list (path, size, deletionVector,
// fileConstantValues), with no data read. Lets you exercise/benchmark the metadata phase on tables
// whose data files aren't reachable. Same driver, metadata_only=true.
static unique_ptr<TableRef> DeltaScanMetadataBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.empty()) {
		return nullptr;
	}
	string table_path = input.inputs[0].GetValue<string>();
	int64_t scan_version = -1;
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(kv.first) == "version" && !kv.second.IsNull()) {
			auto v = kv.second.GetValue<int64_t>();
			scan_version = (v < 0) ? -1 : v;
		}
	}
	string sql = delta_sdk::DriveScan(table_path, scan_version, /* metadata_only= */ true, context);
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw IOException("delta_scan_metadata: expected a single SELECT statement from plan lowering");
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

// in_out stub for delta_load: never executed (bind_operator routes the call to PhysicalDeltaLoad,
// whose source uses function.function). It exists only so the binder classifies delta_load as a
// table-in-out function and accepts the `n7` relation as a subquery argument.
static OperatorResultType DeltaLoadInOutStub(ExecutionContext &, TableFunctionInput &, DataChunk &, DataChunk &) {
	throw InternalException("delta_load in_out_function should never execute (bind_operator routes to the operator)");
}

// Build a faithful-Load read column from a resolved file_schema type, recursively. The column carries
// the PHYSICAL name (from file_schema, which plan_to_sql already emits as the physical/columnMapping name)
// and, when column mapping is enabled, an INTEGER Delta field-id identifier consumed from `field_ids` in
// DFS pre-order (the order plan_to_sql emits them) — top-level AND nested — so the parquet read maps every
// level by field id and emits physical names (the kernel's terminal Transform renames physical->logical).
// `fid_idx` advances across the whole subtree. STRUCT children recurse; LIST/MAP elements are left as-is
// (the Transform passes those through without renaming their elements). For non-column-mapping tables
// field_ids is empty/all-NULL, so columns map by physical==logical name.
static DeltaMultiFileColumnDefinition BuildLoadColumn(const string &name, const LogicalType &type,
                                                      const vector<Value> &field_ids, idx_t &fid_idx) {
	DeltaMultiFileColumnDefinition col(name, type, /*nullable=*/true);
	if (fid_idx < field_ids.size() && !field_ids[fid_idx].IsNull()) {
		col.identifier = Value::INTEGER(field_ids[fid_idx].GetValue<int32_t>());
	}
	fid_idx++;
	if (type.id() == LogicalTypeId::STRUCT) {
		auto &child_types = StructType::GetChildTypes(type);
		for (auto &ct : child_types) {
			col.children.push_back(BuildLoadColumn(ct.first, ct.second, field_ids, fid_idx));
		}
	}
	col.default_expression = make_uniq<ConstantExpression>(Value(type));
	return col;
}

// delta_load(<relation>, file_type, file_schema, base_url, path_column, dv_column, dv_kind,
// metadata_derived, field_ids, version): the faithful kernel Load IR node as a DuckDB table function. The
// relation streams one file-descriptor row per file (scan_file_row); the operator reads each file (its path
// resolved against base_url as a path prefix), reads the columns in file_schema (mapped by field_ids),
// broadcasts the metadata_derived columns onto every output row, and applies the deletion vector if present.
//
// This is SNAPSHOT-FREE: a Load can be a commit/checkpoint/sidecar/data Load whose base_url may not even be
// a Delta table (a sidecar dir is not), so we must NOT open base_url as a Delta snapshot. Everything the
// snapshot would have provided is passed explicitly by the lowering — file_schema (the physical read
// schema), field_ids (column-mapping identifiers), base_url, file_type, metadata_derived — so we build the
// read schema directly and hand a pre-populated DeltaMultiFileList to the multi-file bind (injected via a
// fresh DeltaFunctionInfo so CreateFileList returns it). The file list is then driven by PhysicalDeltaLoad's
// streaming sink, not by a kernel scan.
static unique_ptr<LogicalOperator> DeltaLoadBindOperator(ClientContext &context, TableFunctionBindInput &input,
                                                         idx_t bind_index, vector<string> &return_names) {
	auto &table_function = input.table_function;
	string base_url;
	string file_type = "parquet";
	string file_schema_str;
	Value field_ids_val;
	for (auto &kv : input.named_parameters) {
		auto k = StringUtil::Lower(kv.first);
		if (kv.second.IsNull()) {
			continue;
		}
		if (k == "base_url") {
			base_url = kv.second.GetValue<string>();
		} else if (k == "file_type") {
			file_type = StringUtil::Lower(kv.second.GetValue<string>());
		} else if (k == "file_schema") {
			file_schema_str = kv.second.GetValue<string>();
		} else if (k == "field_ids") {
			field_ids_val = kv.second;
		}
	}
	if (file_type != "parquet" && file_type != "json") {
		throw NotImplementedException("delta_load: unsupported file_type '%s' (expected 'parquet' or 'json')",
		                              file_type);
	}
	if (base_url.empty()) {
		throw InvalidInputException("delta_load: required named parameter 'base_url' is missing");
	}
	if (file_schema_str.empty()) {
		throw InvalidInputException("delta_load: required named parameter 'file_schema' is missing");
	}

	// Build the read schema directly from file_schema (NO snapshot). These names are already the physical
	// (columnMapping) names that plan_to_sql emitted; field_ids carry the Delta column-mapping ids for the
	// by-field-id parquet read.
	auto lp = file_schema_str.find('(');
	auto rp = file_schema_str.rfind(')');
	if (lp == string::npos || rp == string::npos || rp <= lp) {
		throw IOException("delta_load: malformed file_schema '%s'", file_schema_str);
	}
	auto col_list = Parser::ParseColumnList(file_schema_str.substr(lp + 1, rp - lp - 1));
	vector<std::pair<string, LogicalType>> fs;
	for (auto &col : col_list.Logical()) {
		fs.emplace_back(col.Name(), col.Type());
	}
	// Parser::ParseColumnList leaves nested type ids unresolved; resolve through the binder so nested
	// struct field names/types are reliable when we recurse them in BuildLoadColumn.
	if (input.binder) {
		for (auto &p : fs) {
			input.binder->BindLogicalType(p.second);
		}
	}
	vector<Value> field_ids;
	if (!field_ids_val.IsNull()) {
		field_ids = ListValue::GetChildren(field_ids_val);
	}

	vector<DeltaMultiFileColumnDefinition> provided_columns;
	{
		idx_t fid_idx = 0; // DFS cursor into field_ids (advances across each column's subtree)
		for (auto &p : fs) {
			provided_columns.push_back(BuildLoadColumn(p.first, p.second, field_ids, fid_idx));
		}
	}

	// The output schema mirrors the read schema; we will append metadata_derived (broadcast) columns to
	// both the bind output and the provided schema below.
	vector<LogicalType> return_types;
	return_names.clear();
	for (auto &col : provided_columns) {
		return_names.push_back(col.name);
		return_types.push_back(col.type);
	}

	// Faithful Load: append the metadata_derived columns to the output schema as constant (broadcast)
	// columns. Their types come from the input relation by name; the operator broadcasts each value
	// per file via the reader's constant_map (DeltaMultiFileReader::FinalizeBind + BuildFileEntry,
	// which stores fileConstantValues/path in the per-file partition_map). The terminal Transform then
	// extracts partition values from the broadcast fileConstantValues, so partition columns never need
	// to appear in the read schema.
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(kv.first) != "metadata_derived" || kv.second.IsNull()) {
			continue;
		}
		for (auto &mv : ListValue::GetChildren(kv.second)) {
			string mname = mv.GetValue<string>();
			LogicalType mtype = LogicalType::VARCHAR;
			bool found = false;
			for (idx_t c = 0; c < input.input_table_names.size(); c++) {
				if (input.input_table_names[c] == mname) {
					mtype = input.input_table_types[c];
					found = true;
					break;
				}
			}
			if (!found) {
				throw IOException("delta_load: metadata_derived column '%s' not found in input relation", mname);
			}
			return_types.push_back(mtype);
			return_names.push_back(mname);
			// Recurse struct children (the mapper needs them); no field ids for broadcast columns.
			vector<Value> no_field_ids;
			idx_t md_fid = 0;
			provided_columns.push_back(BuildLoadColumn(mname, mtype, no_field_ids, md_fid));
		}
	}

	// Construct the snapshot-free file list carrying the explicit read schema, then inject it into the
	// multi-file bind via a fresh DeltaFunctionInfo (CreateFileList returns reader->snapshot when set).
	// base_url is the path prefix every file resolves against (BuildFileEntry resolves against GetPath()).
	auto provided_list = make_shared_ptr<DeltaMultiFileList>(context, base_url, DConstants::INVALID_INDEX);
	provided_list->SetProvidedSchema(provided_columns, /*partition_cols=*/{});
	provided_list->MarkStreamingPopulated();

	auto load_info = make_shared_ptr<DeltaFunctionInfo>();
	load_info->snapshot = provided_list;
	load_info->table_name = base_url;
	TableFunction inner_function = table_function;
	inner_function.function_info = load_info;

	vector<Value> inner_inputs;
	inner_inputs.emplace_back(Value(base_url));
	named_parameter_map_t inner_named;
	vector<LogicalType> inner_in_types;
	vector<string> inner_in_names;
	TableFunctionBindInput inner(inner_inputs, inner_named, inner_in_types, inner_in_names, load_info.get(),
	                             input.binder.get(), inner_function, input.ref);
	vector<string> inner_names;
	vector<LogicalType> inner_types;
	auto bind_data = inner_function.bind(context, inner, inner_types, inner_names);
	virtual_column_map_t virtual_columns;
	if (inner_function.get_virtual_columns) {
		virtual_columns = inner_function.get_virtual_columns(context, bind_data.get());
	}
	auto &mf_bind = bind_data->Cast<MultiFileBindData>();

	// When the read schema carries Delta field ids (column mapping is enabled), resolve the parquet read
	// by FIELD ID first, then by name (MultiFileColumnMappingMode::BY_FIELD_ID_OR_NAME). Field id is
	// authoritative — it reads the right column even when physical names are reused/swapped — and the
	// name fallback covers files written before an in-place column-mapping upgrade (which carry no field
	// id). InitializeReader stamps the field ids onto the physical-named global columns for the match.
	bool has_field_ids = false;
	for (auto &fid : field_ids) {
		if (!fid.IsNull()) {
			has_field_ids = true;
			break;
		}
	}
	if (has_field_ids) {
		mf_bind.reader_bind.mapping = MultiFileColumnMappingMode::BY_FIELD_ID_OR_NAME;
	}

	auto op = make_uniq<LogicalDeltaGet>(bind_index, inner_function, std::move(bind_data), return_types, return_names,
	                                     std::move(virtual_columns), input.inputs, input.named_parameters);
	// Find the scan_file_row columns in the (binder-attached) input relation's schema, by name.
	for (idx_t c = 0; c < input.input_table_names.size(); c++) {
		const auto &nm = input.input_table_names[c];
		if (nm == "path") {
			op->build_path_col = c;
		} else if (nm == "fileConstantValues") {
			op->build_fcv_col = c;
		} else if (nm == "deletionVector") {
			op->build_dv_col = c;
		}
	}
	if (op->build_path_col == DConstants::INVALID_INDEX) {
		throw IOException("delta_load: input relation is missing the 'path' column");
	}
	return std::move(op);
}

TableFunctionSet DeltaFunctions::GetDeltaLoadFunction(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	ExtensionHelper::AutoLoadExtension(instance, "parquet");
	auto &parquet_scan = loader.GetTableFunction("parquet_scan");
	auto parquet_scan_copy = parquet_scan.functions;

	// A TABLE-parameter function must have exactly one overload; keep the first parquet scan overload
	// (its function.function = the parallel MultiFileScan, which the operator uses at execution).
	TableFunction fn = parquet_scan_copy.functions[0];
	fn.arguments = {LogicalType::TABLE};
	fn.varargs = LogicalType::INVALID;
	fn.get_multi_file_reader = DeltaMultiFileReader::CreateInstance;
	fn.serialize = DeltaScanSerialize;
	fn.deserialize = DeltaScanDeserialize;
	fn.in_out_function = DeltaLoadInOutStub; // accept the relation argument (n7) as a subquery
	fn.bind_operator = DeltaLoadBindOperator;
	fn.bind_replace = nullptr;
	fn.statistics = nullptr;
	fn.table_scan_progress = nullptr;
	fn.get_bind_info = nullptr;
	fn.get_virtual_columns = DeltaVirtualColumns;
	fn.late_materialization = false;
	fn.to_string = DeltaFunctionToString;
	fn.named_parameters.clear();
	fn.named_parameters["file_type"] = LogicalType::VARCHAR;
	fn.named_parameters["file_schema"] = LogicalType::VARCHAR;
	fn.named_parameters["base_url"] = LogicalType::VARCHAR;
	fn.named_parameters["path_column"] = LogicalType::VARCHAR;
	fn.named_parameters["dv_column"] = LogicalType::VARCHAR;
	fn.named_parameters["dv_kind"] = LogicalType::VARCHAR;
	fn.named_parameters["metadata_derived"] = LogicalType::LIST(LogicalType::VARCHAR);
	fn.named_parameters["field_ids"] = LogicalType::LIST(LogicalType::INTEGER);
	fn.named_parameters["version"] = LogicalType::BIGINT;
	fn.name = "delta_load";

	TableFunctionSet result("delta_load");
	result.AddFunction(fn);
	return result;
}

TableFunctionSet DeltaFunctions::GetDeltaScanFunction(ExtensionLoader &loader) {
	// Parquet extension needs to be loaded for this to make sense
	auto &instance = loader.GetDatabaseInstance();
	ExtensionHelper::AutoLoadExtension(instance, "parquet");

	// The delta_scan function is constructed by grabbing the parquet scan from the Catalog, then injecting the
	// DeltaMultiFileReader into it to create a Delta-based multi file read
	auto &parquet_scan = loader.GetTableFunction("parquet_scan");
	auto parquet_scan_copy = parquet_scan.functions;

	for (auto &function : parquet_scan_copy.functions) {
		// Register the MultiFileReader as the driver for reads
		function.get_multi_file_reader = DeltaMultiFileReader::CreateInstance;

		// Unset all of these: they are either broken, very inefficient.
		// TODO: implement/fix these
		function.serialize = DeltaScanSerialize;
		function.deserialize = DeltaScanDeserialize;
		// Plan-based scan: custom operator injection (gated by DELTA_KERNEL_PLAN_OP at bind time).
		function.bind_operator = DeltaScanBindOperator;
		// Plan-based scan (P2): whole-plan SQL string with delta_load TVF (gated by
		// DELTA_KERNEL_PLAN_SQL_TVF). bind_operator runs first and returns nullptr unless its own gate
		// is set, so this is reached when only the SQL-TVF gate is on.
		function.bind_replace = DeltaScanBindReplace;
		function.statistics = nullptr;
		function.table_scan_progress = nullptr;
		function.get_bind_info = nullptr;
		function.get_virtual_columns = DeltaVirtualColumns;
		function.late_materialization = false;

		function.to_string = DeltaFunctionToString;

		// Schema param is just confusing here
		function.named_parameters.erase("schema");

		function.named_parameters["pushdown_partition_info"] = LogicalType::BOOLEAN;
		function.named_parameters["pushdown_filters"] = LogicalType::VARCHAR;
		function.named_parameters["log_tail"] = KernelUtils::GetLogPathType();
		// Time travel: read the snapshot at this version (acceptance-workload read specs).
		function.named_parameters["version"] = LogicalType::BIGINT;

		function.name = "delta_scan";
	}

	parquet_scan_copy.name = "delta_scan";
	return parquet_scan_copy;
}

// delta_scan_metadata(path, [version]): the metadata-only scan. Always drives the kernel scan SM
// (no env gate) and returns the surviving-file list — DuckDB owns the loop; the kernel's
// metadata-only SM terminates at the file list, so no data is read.
TableFunctionSet DeltaFunctions::GetDeltaScanMetadataFunction(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	ExtensionHelper::AutoLoadExtension(instance, "parquet");
	auto &parquet_scan = loader.GetTableFunction("parquet_scan");
	auto parquet_scan_copy = parquet_scan.functions;
	for (auto &function : parquet_scan_copy.functions) {
		function.get_multi_file_reader = DeltaMultiFileReader::CreateInstance;
		function.serialize = DeltaScanSerialize;
		function.deserialize = DeltaScanDeserialize;
		function.bind_operator = nullptr;
		function.bind_replace = DeltaScanMetadataBindReplace;
		function.statistics = nullptr;
		function.table_scan_progress = nullptr;
		function.get_bind_info = nullptr;
		function.get_virtual_columns = DeltaVirtualColumns;
		function.late_materialization = false;
		function.to_string = DeltaFunctionToString;
		function.named_parameters.erase("schema");
		function.named_parameters["version"] = LogicalType::BIGINT;
		function.name = "delta_scan_metadata";
	}
	parquet_scan_copy.name = "delta_scan_metadata";
	return parquet_scan_copy;
}

} // namespace duckdb
