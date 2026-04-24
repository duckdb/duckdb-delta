//===----------------------------------------------------------------------===//
//                         DuckDB
//
// delta_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
class ExtensionLoader;

class DeltaBaseMetadataFunction : public TableFunction {
public:
	DeltaBaseMetadataFunction(string name, table_function_bind_t bind);
};

class DeltaFileListFunction : public DeltaBaseMetadataFunction {
public:
	DeltaFileListFunction();
};

class DeltaDomainMetadataFunction : public DeltaBaseMetadataFunction {
public:
	DeltaDomainMetadataFunction();
};

struct MetadataBindData : public TableFunctionData {
	MetadataBindData() {
	}

	vector<vector<Value>> rows;
};

class TableFunction;

class DeltaFunctions {
public:
	static vector<TableFunctionSet> GetTableFunctions(ExtensionLoader &loader);
	static vector<ScalarFunctionSet> GetScalarFunctions(ExtensionLoader &loader);

private:
	//! Table Functions
	static TableFunctionSet GetDeltaScanFunction(ExtensionLoader &loader);
	static TableFunctionSet GetDeltaFileListFunction(ExtensionLoader &loader);
	static TableFunctionSet GetDeltaDomainMetadataFunction(ExtensionLoader &loader);

	//! Scalar Functions
	static ScalarFunctionSet GetExpressionFunction(ExtensionLoader &loader);

	static ScalarFunctionSet GetWriteFileFunction(ExtensionLoader &loader);

	static vector<TableFunction> GetTransactionIdempotencyHelpers(DatabaseInstance &instance);
};
} // namespace duckdb
