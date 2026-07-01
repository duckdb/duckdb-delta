#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

#include "delta_utils.hpp"
#include "delta_functions.hpp"

namespace duckdb {

static void AddTestExpressions(vector<Value> &result, vector<unique_ptr<ParsedExpression>> &parsed_expressions) {
	if (parsed_expressions.size() != 1) {
		throw InternalException("Unexpected result: expected single expression");
	}

	auto &expression = parsed_expressions.back();
	if (expression->GetExpressionType() == ExpressionType::FUNCTION) {
		for (auto &arg : expression->Cast<FunctionExpression>().GetArguments()) {
			result.push_back(arg.GetExpression().ToString());
		}
	} else if (expression->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &expr : expression->Cast<ConjunctionExpression>().GetChildren()) {
			result.push_back(expr->ToString());
		}
	} else {
		throw InternalException("Unexpected result: expected top level struct or conjunction");
	}
}

static void GetDeltaTestExpression(DataChunk &input, ExpressionState &state, Vector &output) {
	output.SetVectorType(VectorType::CONSTANT_VECTOR);
	vector<Value> result_to_string;

	auto kernel_testing_expr = ffi::get_testing_kernel_expression();
	ExpressionVisitor visitor;
	auto parsed_expressions = visitor.VisitKernelExpression(&kernel_testing_expr);
	AddTestExpressions(result_to_string, *parsed_expressions);

	auto kernel_testing_pred = ffi::get_testing_kernel_predicate();
	ExpressionVisitor visitor_pred;
	auto parsed_pred = visitor_pred.VisitKernelPredicate(&kernel_testing_pred);
	AddTestExpressions(result_to_string, *parsed_pred);

	output.SetValue(0, Value::LIST(result_to_string));
};

ScalarFunctionSet DeltaFunctions::GetExpressionFunction(ExtensionLoader &loader) {
	ScalarFunctionSet result;
	result.SetName("get_delta_test_expression");

	ScalarFunction getvar({}, LogicalType::LIST(LogicalType::VARCHAR), GetDeltaTestExpression, nullptr, nullptr);
	result.AddFunction(getvar);

	return result;
}

} // namespace duckdb
