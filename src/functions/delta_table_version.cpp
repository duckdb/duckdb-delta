#include "delta_functions.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct DeltaTableVersionBindData : public FunctionData {
	idx_t version;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<DeltaTableVersionBindData>();
		result->version = version;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		return version == other_p.Cast<DeltaTableVersionBindData>().version;
	}
};

static unique_ptr<FunctionData> DeltaTableVersionBind(ClientContext &context, ScalarFunction &bound_function,
                                                      vector<unique_ptr<Expression>> &arguments) {
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("delta_table_version: path argument must be a constant");
	}
	auto path = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).ToString();
	auto file_list = make_uniq<DeltaMultiFileList>(context, path, DConstants::INVALID_INDEX);
	auto bind_data = make_uniq<DeltaTableVersionBindData>();
	bind_data->version = file_list->GetVersion();
	return std::move(bind_data);
}

static void DeltaTableVersionExecute(DataChunk &input, ExpressionState &state, Vector &output) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<DeltaTableVersionBindData>();
	output.SetVectorType(VectorType::CONSTANT_VECTOR);
	output.SetValue(0, Value::UBIGINT(bind_data.version));
}

ScalarFunctionSet DeltaFunctions::GetDeltaTableVersionFunction(ExtensionLoader &loader) {
	ScalarFunctionSet result;
	result.name = "delta_table_version";
	result.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR}, LogicalType::UBIGINT, DeltaTableVersionExecute, DeltaTableVersionBind));
	return result;
}

} // namespace duckdb
