#include "delta_functions.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/function/table_function.hpp"

namespace duckdb {

static unique_ptr<FunctionData> DeltaVersionBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto input_string = input.inputs[0].ToString();

	names.emplace_back("version");
	return_types.emplace_back(LogicalType::UBIGINT);

	// Load the snapshot at HEAD and read its version
	auto file_list = make_uniq<DeltaMultiFileList>(context, input_string, DConstants::INVALID_INDEX);
	auto version = file_list->GetVersion();

	auto result = make_uniq<MetadataBindData>();
	result->rows.push_back({Value::UBIGINT(version)});
	return std::move(result);
}

DeltaVersionFunction::DeltaVersionFunction() : DeltaBaseMetadataFunction("delta_version", DeltaVersionBind) {
}

TableFunctionSet DeltaFunctions::GetDeltaVersionFunction(ExtensionLoader &loader) {
	TableFunctionSet function_set("delta_version");
	function_set.AddFunction(DeltaVersionFunction());
	return function_set;
}

} // namespace duckdb
