#include "delta_functions.hpp"

#include "duckdb.hpp"

namespace duckdb {

vector<TableFunctionSet> DeltaFunctions::GetTableFunctions(ExtensionLoader &loader) {
	vector<TableFunctionSet> functions;

	functions.push_back(GetDeltaScanFunction(loader));
	functions.push_back(GetDeltaFileListFunction(loader));
	functions.push_back(GetDeltaDomainMetadataFunction(loader));

	for (const auto &fun : GetTransactionIdempotencyHelpers(loader.GetDatabaseInstance())) {
		functions.push_back(TableFunctionSet(fun));
	}

	return functions;
}

vector<ScalarFunctionSet> DeltaFunctions::GetScalarFunctions(ExtensionLoader &loader) {
	vector<ScalarFunctionSet> functions;

	functions.push_back(GetExpressionFunction(loader));
	functions.push_back(GetWriteFileFunction(loader));

	return functions;
}

} // namespace duckdb
