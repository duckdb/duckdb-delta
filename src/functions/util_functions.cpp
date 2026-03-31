#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

#include "delta_utils.hpp"
#include "delta_functions.hpp"
#include "duckdb/common/local_file_system.hpp"

namespace duckdb {

static void GetWriteDataFunction(DataChunk &input, ExpressionState &state, Vector &output) {
	output.SetVectorType(VectorType::CONSTANT_VECTOR);
	vector<Value> result_to_string;
	LocalFileSystem fs;

	BinaryExecutor::Execute<string_t, string_t, bool>(
	    input.data[0], input.data[1], output, input.size(), [&](string_t path, string_t blob) {
		    auto path_str = path.GetString();
		    auto pos = path_str.find_last_of('/');
		    auto dir = path_str.substr(0, pos);
		    fs.CreateDirectoriesRecursive(dir);

		    auto file =
		        fs.OpenFile(path.GetString(), FileOpenFlags::FILE_FLAGS_FILE_CREATE | FileOpenFlags::FILE_FLAGS_WRITE);
		    file->Write((void *)blob.GetData(), blob.GetSize());
		    return true;
	    });

	output.SetVectorType(VectorType::CONSTANT_VECTOR);
	output.SetValue(0, Value::BOOLEAN(true));
};

ScalarFunctionSet DeltaFunctions::GetWriteFileFunction(ExtensionLoader &loader) {
	ScalarFunctionSet result;
	result.name = "write_blob";

	ScalarFunction write_file({LogicalType::VARCHAR, LogicalType::BLOB}, {LogicalType::BOOLEAN}, GetWriteDataFunction,
	                          nullptr, nullptr);
	result.AddFunction(write_file);

	return result;
}

} // namespace duckdb
