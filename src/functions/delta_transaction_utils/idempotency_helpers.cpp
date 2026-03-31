#include "delta_kernel_ffi.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set.hpp"

#include "delta_utils.hpp"
#include "delta_functions.hpp"
#include "storage/delta_transaction.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "storage/delta_table_entry.hpp"

namespace duckdb {

struct TransactionVersionData : public GlobalTableFunctionState {
	TransactionVersionData() : finished(false) {
	}

	bool finished;
};

struct TransactionSetVersionBindData : public TableFunctionData {
	string app_id;
	idx_t new_version;
	Value expected_version;

	optional_ptr<DeltaTableEntry> delta_table_entry;
};

struct TransactionGetVersionBindData : public TableFunctionData {
	string app_id;
	Value version;

	optional_ptr<DeltaTableEntry> delta_table_entry;
};

static void DeltaGetTransactionVersionFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto &bind_data = data.bind_data->Cast<TransactionGetVersionBindData>();
	auto &global_state = data.global_state->Cast<TransactionVersionData>();
	if (global_state.finished) {
		return;
	}

	auto &delta_table_entry = *bind_data.delta_table_entry;
	auto &snapshot = delta_table_entry.snapshot;
	auto kernel_snapshot = snapshot->snapshot->GetLockingRef();
	auto app_id_kernel_string = KernelUtils::ToDeltaString(bind_data.app_id);
	auto get_app_id_version_result =
	    ffi::get_app_id_version(kernel_snapshot.GetPtr(), app_id_kernel_string, snapshot->extern_engine.get());

	ffi::OptionalValue<int64_t> version_opt;
	auto unpacked_version_result = KernelUtils::TryUnpackResult(get_app_id_version_result, version_opt);
	if (unpacked_version_result.HasError() || version_opt.tag == ffi::OptionalValue<int64_t>::Tag::None) {
		output.SetValue(0, 0, Value());
	} else {
		output.SetValue(0, 0, Value::UBIGINT(version_opt.some._0));
	}
	output.SetCardinality(1);

	global_state.finished = true;
}

static void DeltaSetTransactionVersionFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto &bind_data = data.bind_data->Cast<TransactionSetVersionBindData>();
	auto &global_state = data.global_state->Cast<TransactionVersionData>();
	if (global_state.finished) {
		return;
	}

	// TODO: Attach to transaction
	auto &transaction = DeltaTransaction::Get(context, bind_data.delta_table_entry->catalog);

	transaction.SetTransactionVersion(bind_data.app_id, bind_data.new_version, bind_data.expected_version);

	global_state.finished = true;
}

static unique_ptr<FunctionData> DeltaGetTransactionVersionBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<string> &names) {
	auto res = make_uniq<TransactionGetVersionBindData>();

	auto path = input.inputs[0].GetValue<string>();
	res->app_id = input.inputs[1].GetValue<string>();

	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("version");

	// TODO: support catalog.schema.table format
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, path);
	auto lookup_result = Catalog::GetEntry(context, "", "", lookup, OnEntryNotFound::RETURN_NULL);
	if (lookup_result.get()) {
		res->delta_table_entry = lookup_result->Cast<DeltaTableEntry>();
	} else {
		throw CatalogException("Table not found");
	}

	return std::move(res);
}

static unique_ptr<FunctionData> DeltaSetTransactionVersionBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<string> &names) {
	auto res = make_uniq<TransactionSetVersionBindData>();

	auto path = input.inputs[0].GetValue<string>();
	res->app_id = input.inputs[1].GetValue<string>();
	res->new_version = input.inputs[2].GetValue<idx_t>();
	res->expected_version = input.inputs[3];

	// TODO: support catalog.schema.table format
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, path);
	auto lookup_result = Catalog::GetEntry(context, "", "", lookup, OnEntryNotFound::RETURN_NULL);
	if (lookup_result.get()) {
		res->delta_table_entry = lookup_result->Cast<DeltaTableEntry>();
	} else {
		throw CatalogException("Table not found");
	}

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return std::move(res);
}

static unique_ptr<GlobalTableFunctionState> TransactionInitGlobalState(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	return make_uniq<TransactionVersionData>();
}

vector<TableFunction> DeltaFunctions::GetTransactionIdempotencyHelpers(DatabaseInstance &instance) {
	vector<TableFunction> result;
	result.push_back(TableFunction("delta_get_transaction_version", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                               DeltaGetTransactionVersionFunction, DeltaGetTransactionVersionBind,
	                               TransactionInitGlobalState));
	result.push_back(
	    TableFunction("delta_set_transaction_version",
	                  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT},
	                  DeltaSetTransactionVersionFunction, DeltaSetTransactionVersionBind, TransactionInitGlobalState));
	return result;
}

} // namespace duckdb
