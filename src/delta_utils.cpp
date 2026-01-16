#include "delta_utils.hpp"

#include <list>

#include "delta_log_types.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"

#include "duckdb.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "include/delta_kernel_ffi.hpp"

namespace duckdb {

void ExpressionVisitor::VisitComparisonExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);

	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}

	auto &lhs = children->at(0);
	auto &rhs = children->at(1);
	unique_ptr<ComparisonExpression> expression =
	    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHAN, std::move(lhs), std::move(rhs));
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

ffi::EngineExpressionVisitor ExpressionVisitor::CreateVisitor(ExpressionVisitor &state) {
	ffi::EngineExpressionVisitor visitor;

	visitor.data = &state;
	visitor.make_field_list = (uintptr_t(*)(void *, uintptr_t)) & MakeFieldList;

	// Templated primitive functions
	visitor.visit_literal_bool = VisitPrimitiveLiteralBool;
	visitor.visit_literal_byte = VisitPrimitiveLiteralByte;
	visitor.visit_literal_short = VisitPrimitiveLiteralShort;
	visitor.visit_literal_int = VisitPrimitiveLiteralInt;
	visitor.visit_literal_long = VisitPrimitiveLiteralLong;
	visitor.visit_literal_float = VisitPrimitiveLiteralFloat;
	visitor.visit_literal_double = VisitPrimitiveLiteralDouble;

	visitor.visit_literal_decimal = VisitDecimalLiteral;

	// Custom Implementations
	visitor.visit_literal_timestamp = &VisitTimestampLiteral;
	visitor.visit_literal_timestamp_ntz = &VisitTimestampNtzLiteral;
	visitor.visit_literal_date = &VisitDateLiteral;

	visitor.visit_literal_string = &VisitStringLiteral;

	visitor.visit_literal_binary = &VisitBinaryLiteral;
	visitor.visit_literal_null = &VisitNullLiteral;
	visitor.visit_literal_array = &VisitArrayLiteral;

	visitor.visit_and = VisitVariadicExpression<ExpressionType::CONJUNCTION_AND, ConjunctionExpression>();
	visitor.visit_or = VisitVariadicExpression<ExpressionType::CONJUNCTION_OR, ConjunctionExpression>();

	visitor.visit_lt = VisitBinaryExpression<ExpressionType::COMPARE_LESSTHAN, ComparisonExpression>();
	visitor.visit_gt = VisitBinaryExpression<ExpressionType::COMPARE_GREATERTHAN, ComparisonExpression>();

	visitor.visit_eq = VisitBinaryExpression<ExpressionType::COMPARE_EQUAL, ComparisonExpression>();
	visitor.visit_distinct = VisitBinaryExpression<ExpressionType::COMPARE_DISTINCT_FROM, ComparisonExpression>();

	visitor.visit_in = VisitVariadicExpression<ExpressionType::COMPARE_IN, OperatorExpression>();

	visitor.visit_add = VisitAdditionExpression;
	visitor.visit_minus = VisitSubctractionExpression;
	visitor.visit_multiply = VisitMultiplyExpression;
	visitor.visit_divide = VisitDivideExpression;
    visitor.visit_coalesce = VisitCoalesceExpression;

	visitor.visit_column = VisitColumnExpression;
	visitor.visit_struct_expr = VisitStructExpression;

    visitor.visit_transform_expr = VisitTransformExpression;
    visitor.visit_field_transform = VisitFieldTransform;

	visitor.visit_literal_struct = VisitStructLiteral;

	visitor.visit_not = VisitNotExpression;
	visitor.visit_is_null = VisitIsNullExpression;

	visitor.visit_literal_map = VisitLiteralMap;

    visitor.visit_opaque_expr = VisitOpaqueExpression;
    visitor.visit_opaque_pred = VisitOpaquePredicate;

    visitor.visit_unknown = VisitUnknown;

	return visitor;
}

unique_ptr<vector<unique_ptr<ParsedExpression>>>
ExpressionVisitor::VisitKernelExpression(const ffi::Expression *expression) {
	ExpressionVisitor state;
	auto visitor = CreateVisitor(state);

	uintptr_t result = ffi::visit_expression_ref(expression, &visitor);

	if (state.error.HasError()) {
		state.error.Throw();
	}

	return state.TakeFieldList(result);
}

unique_ptr<vector<unique_ptr<ParsedExpression>>>
ExpressionVisitor::VisitKernelExpression(const ffi::Handle<ffi::SharedExpression> *expression) {
	ExpressionVisitor state;
	auto visitor = CreateVisitor(state);

	uintptr_t result = ffi::visit_expression(expression, &visitor);

	if (state.error.HasError()) {
		state.error.Throw();
	}

	return state.TakeFieldList(result);
}

unique_ptr<vector<unique_ptr<ParsedExpression>>>
ExpressionVisitor::VisitKernelPredicate(const ffi::Handle<ffi::SharedPredicate> *predicate) {
	ExpressionVisitor state;
	auto visitor = CreateVisitor(state);

	uintptr_t result = ffi::visit_predicate(predicate, &visitor);

	if (state.error.HasError()) {
		state.error.Throw();
	}

	return state.TakeFieldList(result);
}

void ExpressionVisitor::VisitAdditionExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}
	unique_ptr<ParsedExpression> expression =
	    make_uniq<FunctionExpression>("+", std::move(*children), nullptr, nullptr, false, true);
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitSubctractionExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}
	unique_ptr<ParsedExpression> expression =
	    make_uniq<FunctionExpression>("-", std::move(*children), nullptr, nullptr, false, true);
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitDivideExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}
	unique_ptr<ParsedExpression> expression =
	    make_uniq<FunctionExpression>("/", std::move(*children), nullptr, nullptr, false, true);
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitCoalesceExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
    state_cast->error = ErrorData( ExceptionType::NOT_IMPLEMENTED, "Coalesce expression is not supported yet");
}

void ExpressionVisitor::VisitMultiplyExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}
	unique_ptr<ParsedExpression> expression =
	    make_uniq<FunctionExpression>("*", std::move(*children), nullptr, nullptr, false, true);
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitPrimitiveLiteralBool(void *state, uintptr_t sibling_list_id, bool value) {
	auto expression = make_uniq<ConstantExpression>(Value::BOOLEAN(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitPrimitiveLiteralByte(void *state, uintptr_t sibling_list_id, int8_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::TINYINT(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitPrimitiveLiteralShort(void *state, uintptr_t sibling_list_id, int16_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::SMALLINT(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitPrimitiveLiteralInt(void *state, uintptr_t sibling_list_id, int32_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::INTEGER(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitPrimitiveLiteralLong(void *state, uintptr_t sibling_list_id, int64_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::BIGINT(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitPrimitiveLiteralFloat(void *state, uintptr_t sibling_list_id, float value) {
	auto expression = make_uniq<ConstantExpression>(Value::FLOAT(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitPrimitiveLiteralDouble(void *state, uintptr_t sibling_list_id, double value) {
	auto expression = make_uniq<ConstantExpression>(Value::DOUBLE(value));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitTimestampLiteral(void *state, uintptr_t sibling_list_id, int64_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::TIMESTAMPTZ(timestamp_tz_t(value)));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitTimestampNtzLiteral(void *state, uintptr_t sibling_list_id, int64_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::TIMESTAMP(static_cast<timestamp_t>(value)));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitDateLiteral(void *state, uintptr_t sibling_list_id, int32_t value) {
	auto expression = make_uniq<ConstantExpression>(Value::DATE(static_cast<date_t>(value)));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitStringLiteral(void *state, uintptr_t sibling_list_id, ffi::KernelStringSlice value) {
	auto expression = make_uniq<ConstantExpression>(Value(string(value.ptr, value.len)));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitBinaryLiteral(void *state, uintptr_t sibling_list_id, const uint8_t *buffer,
                                           uintptr_t len) {
	auto expression = make_uniq<ConstantExpression>(Value::BLOB(buffer, len));
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitNullLiteral(void *state, uintptr_t sibling_list_id) {
	auto expression = make_uniq<ConstantExpression>(Value());
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}
void ExpressionVisitor::VisitArrayLiteral(void *state, uintptr_t sibling_list_id, uintptr_t child_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_id);
	if (!children) {
		return;
	}
	unique_ptr<ParsedExpression> expression = make_uniq<FunctionExpression>("list_value", std::move(*children));
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitStructLiteral(void *state, uintptr_t sibling_list_id, uintptr_t child_field_list_value,
                                           uintptr_t child_value_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);

	auto children_keys = state_cast->TakeFieldList(child_field_list_value);
	auto children_values = state_cast->TakeFieldList(child_value_list_id);
	if (!children_values || !children_keys) {
		return;
	}

	if (children_values->size() != children_keys->size()) {
		state_cast->error =
		    ErrorData("Size of Keys and Values vector do not match in ExpressionVisitor::VisitStructLiteral");
		return;
	}

	for (idx_t i = 0; i < children_keys->size(); i++) {
		(*children_values)[i]->alias = (*children_keys)[i]->ToString();
	}

	unique_ptr<ParsedExpression> expression = make_uniq<FunctionExpression>("struct_pack", std::move(*children_values));
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitNotExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}
	unique_ptr<ParsedExpression> expression =
	    make_uniq<FunctionExpression>("NOT", std::move(*children), nullptr, nullptr, false, true);
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitIsNullExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);
	auto children = state_cast->TakeFieldList(child_list_id);
	if (!children) {
		return;
	}

	children->push_back(make_uniq<ConstantExpression>(Value()));
	unique_ptr<ParsedExpression> expression =
	    make_uniq<FunctionExpression>("IS", std::move(*children), nullptr, nullptr, false, true);
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitLiteralMap(void *state, uintptr_t sibling_list_id, uintptr_t key_list_id,
                                        uintptr_t value_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);

	auto key_children = state_cast->TakeFieldList(key_list_id);
	if (!key_children) {
		return;
	}
	auto value_children = state_cast->TakeFieldList(value_list_id);
	if (!value_children) {
		return;
	}

	vector<Value> key_values;
	LogicalType key_type;
	for (const auto &key_field : *key_children) {
		if (key_field->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			state_cast->error =
			    ErrorData("DuckDB only supports parsing Map literals from delta kernel that consist for constants!");
			return;
		}
		key_values.push_back(key_field->Cast<ConstantExpression>().value);
		key_type = key_field->Cast<ConstantExpression>().value.type();
	}

	vector<Value> value_values;
	LogicalType value_type;
	for (const auto &value_field : *value_children) {
		if (value_field->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			state_cast->error =
			    ErrorData("DuckDB only supports parsing Map literals from delta kernel that consist for constants!");
			return;
		}
		value_values.push_back(value_field->Cast<ConstantExpression>().value);
		value_type = value_field->Cast<ConstantExpression>().value.type();
	}

	unique_ptr<ParsedExpression> expression =
	    make_uniq<ConstantExpression>(Value::MAP(key_type, value_type, key_values, value_values));
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitOpaqueExpression(void *data,
                                uintptr_t sibling_list_id,
                                ffi::Handle<ffi::SharedOpaqueExpressionOp> op,
                                uintptr_t child_list_id) {
    auto state_cast = static_cast<ExpressionVisitor *>(data);
    auto children = state_cast->TakeFieldList(child_list_id);
    if (!children) {
        return;
    }
    unique_ptr<ParsedExpression> expression =
        make_uniq<FunctionExpression>("delta_kernel_opaque_expression", std::move(*children), nullptr, nullptr, false, false);

    // TODO: handle DuckDB opaque expressions here
    state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitOpaquePredicate(void *data,
                              uintptr_t sibling_list_id,
                              ffi::Handle<ffi::SharedOpaquePredicateOp> op,
                              uintptr_t child_list_id) {
    auto state_cast = static_cast<ExpressionVisitor *>(data);
    auto children = state_cast->TakeFieldList(child_list_id);
    if (!children) {
        return;
    }
    unique_ptr<ParsedExpression> expression =
        make_uniq<FunctionExpression>("delta_kernel_opaque_predicate", std::move(*children), nullptr, nullptr, false, false);

    // TODO: handle DuckDB opaque predicatae here
    state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitUnknown(void *data, uintptr_t sibling_list_id, ffi::KernelStringSlice name) {
    auto state_cast = static_cast<ExpressionVisitor *>(data);
    vector<unique_ptr<ParsedExpression>> children;
    // TODO:
    // auto name_str = KernelUtils::FromDeltaString(name);
    // auto expr_string = StringUtil::Format("delta_kernel_unknown(\"%s\")", name_str);
    unique_ptr<ParsedExpression> expression =
        make_uniq<FunctionExpression>("delta_kernel_unknown", std::move(children), nullptr, nullptr, false, true);

    state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitDecimalLiteral(void *state, uintptr_t sibling_list_id, int64_t value_ms, uint64_t value_ls,
                                            uint8_t precision, uint8_t scale) {
	try {
		Value decimal_value;
		if (precision < Decimal::MAX_WIDTH_INT64) {
		    auto cast = Value::HUGEINT({value_ms, value_ls}).DefaultCastAs(LogicalType::BIGINT);
			decimal_value = Value::DECIMAL(cast.GetValue<int64_t>(), precision, scale);
		} else {
			decimal_value = Value::DECIMAL({value_ms, value_ls}, precision, scale);
		}
		auto expression = make_uniq<ConstantExpression>(decimal_value);
		static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
	} catch (Exception &e) {
		static_cast<ExpressionVisitor *>(state)->error = ErrorData(e);
	}
}

void ExpressionVisitor::VisitColumnExpression(void *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name) {
	auto col_ref_string = string(name.ptr, name.len);

	// Delta ColRefs are sometimes backtick-ed
	if (col_ref_string[0] == '`' && col_ref_string[col_ref_string.size() - 1] == '`') {
		col_ref_string = col_ref_string.substr(1, col_ref_string.size() - 2);
	}

	auto expression = make_uniq<ColumnRefExpression>(col_ref_string);
	static_cast<ExpressionVisitor *>(state)->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitStructExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
	auto state_cast = static_cast<ExpressionVisitor *>(state);

	auto children_values = state_cast->TakeFieldList(child_list_id);
	if (!children_values) {
		return;
	}

	unique_ptr<ParsedExpression> expression = make_uniq<FunctionExpression>("struct_pack", std::move(*children_values));
	state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitTransformExpression(void *state, uintptr_t sibling_list_id, uintptr_t input_path_list_id, uintptr_t child_list_id) {
    auto state_cast = static_cast<ExpressionVisitor *>(state);

    auto children_values = state_cast->TakeFieldList(child_list_id);
    if (!children_values) {
        return;
    }

    if (input_path_list_id) {
        auto input_path = state_cast->TakeFieldList(input_path_list_id);

        if (input_path->size() != 1) {
            state_cast->error = ErrorData("Expected exactly one input path for transform expression");
            return;
        }
        children_values->push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("input_path"), std::move(input_path->front())));
    }

    unique_ptr<ParsedExpression> expression = make_uniq<FunctionExpression>("delta_kernel_transform_expression", std::move(*children_values));
    state_cast->AppendToList(sibling_list_id, std::move(expression));
}

void ExpressionVisitor::VisitFieldTransform(void *state, uintptr_t sibling_list_id, const ffi::KernelStringSlice *field_name, uintptr_t expr_list_id, bool is_replace) {
    auto state_cast = static_cast<ExpressionVisitor *>(state);

    unique_ptr<FieldList> children_values;

    if (expr_list_id) {
        children_values = state_cast->TakeFieldList(expr_list_id);
        if (!children_values) {
            return;
        }
    } else {
        children_values = make_uniq<FieldList>();
    }

    // Create is_insert_value
    children_values->push_back(
                    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("is_replace"),
                    make_uniq<ConstantExpression>(Value::BOOLEAN(is_replace))));


    // Create field name expr
    unique_ptr<ParsedExpression> field_name_val;
    if (field_name) {
        string field_name_str = KernelUtils::FromDeltaString(*field_name);
        field_name_val = make_uniq<ConstantExpression>(Value(field_name_str));
    } else {
        field_name_val = make_uniq<ConstantExpression>(Value());
    }
    children_values->push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>("field_name"), std::move(field_name_val)));

    unique_ptr<ParsedExpression> expression = make_uniq<FunctionExpression>("delta_transform_op", std::move(*children_values));
    state_cast->AppendToList(sibling_list_id, std::move(expression));
}

uintptr_t ExpressionVisitor::MakeFieldList(ExpressionVisitor *state, uintptr_t capacity_hint) {
	return state->MakeFieldListImpl(capacity_hint);
}
uintptr_t ExpressionVisitor::MakeFieldListImpl(uintptr_t capacity_hint) {
	uintptr_t id = next_id++;
	auto list = make_uniq<FieldList>();
	if (capacity_hint > 0) {
		list->reserve(capacity_hint);
	}
	inflight_lists.emplace(id, std::move(list));
	return id;
}

void ExpressionVisitor::AppendToList(uintptr_t id, unique_ptr<ParsedExpression> child) {
	auto it = inflight_lists.find(id);
	if (it == inflight_lists.end()) {
		error = ErrorData("ExpressionVisitor::AppendToList could not find " + Value::UBIGINT(id).ToString());
		return;
	}

	it->second->emplace_back(std::move(child));
}

unique_ptr<ExpressionVisitor::FieldList> ExpressionVisitor::TakeFieldList(uintptr_t id) {
	auto it = inflight_lists.find(id);
	if (it == inflight_lists.end()) {
		error = ErrorData("ExpressionVisitor::TakeFieldList could not find " + Value::UBIGINT(id).ToString());
		return nullptr;
	}
	auto rval = std::move(it->second);
	inflight_lists.erase(it);
	return rval;
}

ffi::EngineSchemaVisitor SchemaVisitor::CreateSchemaVisitor(SchemaVisitor &state, bool enable_variant) {
    ffi::EngineSchemaVisitor visitor;

    visitor.data = &state;
    visitor.make_field_list = (uintptr_t(*)(void *, uintptr_t)) & MakeFieldList;
    visitor.visit_struct =
        (void (*)(void *, uintptr_t, ffi::KernelStringSlice, bool, const ffi::CStringMap *metadata, uintptr_t)) &
        VisitStruct;
    visitor.visit_array =
        (void (*)(void *, uintptr_t, ffi::KernelStringSlice, bool, const ffi::CStringMap *metadata, uintptr_t)) &
        VisitArray;
    visitor.visit_map =
        (void (*)(void *, uintptr_t, ffi::KernelStringSlice, bool, const ffi::CStringMap *metadata, uintptr_t)) &
        VisitMap;
    visitor.visit_decimal =
        (void (*)(void *, uintptr_t, ffi::KernelStringSlice, bool, const ffi::CStringMap *metadata, uint8_t, uint8_t)) &
        VisitDecimal;
    visitor.visit_string = VisitSimpleType<LogicalType::VARCHAR>();
    visitor.visit_long = VisitSimpleType<LogicalType::BIGINT>();
    visitor.visit_integer = VisitSimpleType<LogicalType::INTEGER>();
    visitor.visit_short = VisitSimpleType<LogicalType::SMALLINT>();
    visitor.visit_byte = VisitSimpleType<LogicalType::TINYINT>();
    visitor.visit_float = VisitSimpleType<LogicalType::FLOAT>();
    visitor.visit_double = VisitSimpleType<LogicalType::DOUBLE>();
    visitor.visit_boolean = VisitSimpleType<LogicalType::BOOLEAN>();
    visitor.visit_binary = VisitSimpleType<LogicalType::BLOB>();
    visitor.visit_date = VisitSimpleType<LogicalType::DATE>();
    visitor.visit_timestamp = VisitSimpleType<LogicalType::TIMESTAMP_TZ>();
    visitor.visit_timestamp_ntz = VisitSimpleType<LogicalType::TIMESTAMP>();

    if (enable_variant) {
        visitor.visit_variant =  (void (*)(void *data, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
        bool is_nullable, const ffi::CStringMap *metadata)) & VisitVariant<true>;
    } else {
        visitor.visit_variant =  (void (*)(void *data, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
        bool is_nullable, const ffi::CStringMap *metadata)) & VisitVariant<false>;
    }

	return visitor;
}

vector<DeltaMultiFileColumnDefinition> SchemaVisitor::VisitSnapshotSchema(ffi::SharedSnapshot *snapshot, bool enable_variant) {
	SchemaVisitor state;
	auto visitor = CreateSchemaVisitor(state, enable_variant);

	auto schema = logical_schema(snapshot);
	uintptr_t result = visit_schema(schema, &visitor);
	free_schema(schema);

	if (state.error.HasError()) {
		state.error.Throw();
	}

	return state.TakeFieldList(result);
}

vector<DeltaMultiFileColumnDefinition> SchemaVisitor::VisitSnapshotGlobalReadSchema(ffi::SharedScan *scan,
                                                                                  bool logical, bool enable_variant) {
	SchemaVisitor visitor_state;
	auto visitor = CreateSchemaVisitor(visitor_state, enable_variant);

	ffi::Handle<ffi::SharedSchema> schema;
	if (logical) {
		schema = ffi::scan_logical_schema(scan);
	} else {
		schema = ffi::scan_physical_schema(scan);
	}

	uintptr_t result = visit_schema(schema, &visitor);
	free_schema(schema);

	if (visitor_state.error.HasError()) {
		visitor_state.error.Throw();
	}

	return visitor_state.TakeFieldList(result);
}

vector<DeltaMultiFileColumnDefinition> SchemaVisitor::VisitWriteContextSchema(ffi::SharedWriteContext *write_context, bool enable_variant) {
	SchemaVisitor visitor_state;
	auto visitor = CreateSchemaVisitor(visitor_state, enable_variant);
    auto schema = ffi::get_write_schema(write_context);
	uintptr_t result = visit_schema(schema, &visitor);
	free_schema(schema);

	if (visitor_state.error.HasError()) {
		visitor_state.error.Throw();
	}

	return visitor_state.TakeFieldList(result);
}

void SchemaVisitor::VisitDecimal(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                                 bool is_nullable, const ffi::CStringMap *metadata, uint8_t precision, uint8_t scale) {
    auto decimal_type = LogicalType::DECIMAL(precision, scale);
    DeltaMultiFileColumnDefinition decimal_def(KernelUtils::FromDeltaString(name), decimal_type, is_nullable);
    decimal_def.default_expression = make_uniq<ConstantExpression>(Value().DefaultCastAs(decimal_type));

    ApplyDeltaColumnMapping(metadata, decimal_def);

	state->AppendToList(sibling_list_id, name, std::move(decimal_def));
}

uintptr_t SchemaVisitor::MakeFieldList(SchemaVisitor *state, uintptr_t capacity_hint) {
	return state->MakeFieldListImpl(capacity_hint);
}

void SchemaVisitor::VisitStruct(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                                bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id) {
	auto children = state->TakeFieldList(child_list_id);

    child_list_t<LogicalType> children_types;
    for (const auto &child_col_def : children) {
        children_types.push_back({child_col_def.name, child_col_def.type});
    }

    auto struct_type = LogicalType::STRUCT(children_types);
    DeltaMultiFileColumnDefinition struct_def(KernelUtils::FromDeltaString(name), struct_type, is_nullable);
    struct_def.children = std::move(children);
    struct_def.default_expression = make_uniq<ConstantExpression>(Value(struct_type));

    ApplyDeltaColumnMapping(metadata, struct_def);

	state->AppendToList(sibling_list_id, name, std::move(struct_def));
}

void SchemaVisitor::VisitArray(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                               bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id) {
	auto children = state->TakeFieldList(child_list_id);

	D_ASSERT(children.size() == 1);

    auto list_type = LogicalType::LIST(children.front().type);

    DeltaMultiFileColumnDefinition list_def(KernelUtils::FromDeltaString(name), list_type, is_nullable);
    list_def.children.push_back(std::move(children.front()));
    list_def.default_expression = make_uniq<ConstantExpression>(Value(list_type));

    // TODO: kinda wonky, but column mapper uses this
    list_def.children.front().name = "list";

    ApplyDeltaColumnMapping(metadata, list_def);

	state->AppendToList(sibling_list_id, name, std::move(list_def));
}

void SchemaVisitor::VisitMap(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                             bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id) {
	auto children = state->TakeFieldList(child_list_id);

	D_ASSERT(children.size() == 2);

    auto &key = children.front();
    key.name = "key";
    auto & value = children.back();
    value.name = "value";

    auto map_type = LogicalType::MAP(key.type, value.type);
    DeltaMultiFileColumnDefinition map_def(KernelUtils::FromDeltaString(name), map_type, is_nullable);
    map_def.children.push_back(std::move(key));
    map_def.children.push_back(std::move(value));

    map_def.default_expression = make_uniq<ConstantExpression>(Value(map_type));

    ApplyDeltaColumnMapping(metadata, map_def);

	state->AppendToList(sibling_list_id, name, std::move(map_def));
}

uintptr_t SchemaVisitor::MakeFieldListImpl(uintptr_t capacity_hint) {
	uintptr_t id = next_id++;
	auto list = vector<DeltaMultiFileColumnDefinition>();;
	if (capacity_hint > 0) {
		list.reserve(capacity_hint);
	}
	inflight_lists.emplace(id, std::move(list));
	return id;
}

void SchemaVisitor::AppendToList(uintptr_t id, ffi::KernelStringSlice name, DeltaMultiFileColumnDefinition &&child) {
	auto it = inflight_lists.find(id);
	if (it == inflight_lists.end()) {
		error = ErrorData(ExceptionType::INTERNAL, "Unhandled error in SchemaVisitor::AppendToList");
		return;
	}

    // Inject the name
    child.name = string(name.ptr, name.len);

	it->second.emplace_back(std::move(child));
}

vector<DeltaMultiFileColumnDefinition> SchemaVisitor::TakeFieldList(uintptr_t id) {
	auto it = inflight_lists.find(id);
	if (it == inflight_lists.end()) {
		error = ErrorData(ExceptionType::INTERNAL, "Unhandled error in SchemaVisitor::TakeFieldList");
		return vector<DeltaMultiFileColumnDefinition>();
	}
	auto rval = std::move(it->second);
	inflight_lists.erase(it);
	return rval;
}

ffi::EngineError *DuckDBEngineError::AllocateError(ffi::KernelError etype, ffi::KernelStringSlice msg) {
	auto error = new DuckDBEngineError;
	error->etype = etype;
	error->error_message = string(msg.ptr, msg.len);
	return error;
}

string DuckDBEngineError::KernelErrorEnumToString(ffi::KernelError err) {
	const char *KERNEL_ERROR_ENUM_STRINGS[] = {"UnknownError",
	                                           "FFIError",
	                                           "ArrowError",
	                                           "EngineDataTypeError",
	                                           "ExtractError",
	                                           "GenericError",
	                                           "IOErrorError",
	                                           "ParquetError",
	                                           "ObjectStoreError",
	                                           "ObjectStorePathError",
	                                           "ReqwestError",
	                                           "FileNotFoundError",
	                                           "MissingColumnError",
	                                           "UnexpectedColumnTypeError",
	                                           "MissingDataError",
	                                           "MissingVersionError",
	                                           "DeletionVectorError",
	                                           "InvalidUrlError",
	                                           "MalformedJsonError",
	                                           "MissingMetadataError",
	                                           "MissingProtocolError",
	                                           "InvalidProtocolError",
	                                           "MissingMetadataAndProtocolError",
	                                           "ParseError",
	                                           "JoinFailureError",
	                                           "Utf8Error",
	                                           "ParseIntError",
	                                           "InvalidColumnMappingModeError",
	                                           "InvalidTableLocationError",
	                                           "InvalidDecimalError",
	                                           "InvalidStructDataError",
	                                           "InternalError",
	                                           "InvalidExpression",
	                                           "InvalidLogPath",
	                                           "FileAlreadyExists",
	                                           "UnsupportedError",
	                                           "ParseIntervalError",
	                                           "ChangeDataFeedUnsupported",
	                                           "ChangeDataFeedIncompatibleSchema",
	                                           "InvalidCheckpoint"};

	static_assert(sizeof(KERNEL_ERROR_ENUM_STRINGS) / sizeof(char *) - 1 == (int)ffi::KernelError::InvalidCheckpoint,
	              "KernelErrorEnumStrings mismatched with kernel");

	if ((int)err < sizeof(KERNEL_ERROR_ENUM_STRINGS) / sizeof(char *)) {
		return KERNEL_ERROR_ENUM_STRINGS[(int)err];
	}

	return StringUtil::Format("EnumOutOfRange (enum val out of range: %d)", (int)err);
}

string DuckDBEngineError::IntoString() {
	// Make copies before calling delete this
	auto etype_copy = etype;
	auto message_copy = error_message;

	// Consume error by calling delete this (remember this error is created by
	// kernel using AllocateError)
	delete this;
	return StringUtil::Format("DeltaKernel %s (%u): %s", KernelErrorEnumToString(etype_copy), etype_copy, message_copy);
}

ffi::KernelStringSlice KernelUtils::ToDeltaString(const string &str) {
	return {str.data(), str.size()};
}

string KernelUtils::FromDeltaString(const struct ffi::KernelStringSlice slice) {
	return {slice.ptr, slice.len};
}

vector<bool> KernelUtils::FromDeltaBoolSlice(const struct ffi::KernelBoolSlice slice) {
	vector<bool> result;
	result.assign(slice.ptr, slice.ptr + slice.len);
	return result;
}

string KernelUtils::FetchFromStringMap(const ffi::CStringMap *str_map, const string &key) {
    auto res = ffi::get_from_string_map(str_map, ToDeltaString(key), StringAllocationNew);
    string val;
    if (res) {
        val = *(string*)res;
        delete static_cast<string*>(res);
    }
    return val;
}

vector<unique_ptr<ParsedExpression>> &
KernelUtils::UnpackTransformExpression(const vector<unique_ptr<ParsedExpression>> &parsed_expression) {
	if (parsed_expression.size() != 1) {
		throw IOException("Unexpected size of transformation expression returned by delta kernel: %d",
		                  parsed_expression.size());
	}

	const auto &root_expression = parsed_expression.get(0);
	if (root_expression->type != ExpressionType::FUNCTION) {
		throw IOException("Unexpected type of root expression returned by delta kernel: %d", root_expression->type);
	}

	if (root_expression->Cast<FunctionExpression>().function_name != "delta_kernel_transform_expression") {
		throw IOException("Unexpected function of root expression returned by delta kernel: %s",
		                  root_expression->Cast<FunctionExpression>().function_name);
	}

	return root_expression->Cast<FunctionExpression>().children;
}

PredicateVisitor::PredicateVisitor(const vector<DeltaMultiFileColumnDefinition> &columns, optional_ptr<const TableFilterSet> filters) {
	predicate = this;
	visitor = (uintptr_t(*)(void *, ffi::KernelExpressionVisitorState *)) & VisitPredicate;

	if (filters) {
		for (auto &filter : filters->filters) {
			column_filters[columns[filter.first].name] = filter.second.get();
		}
	}
}

// Template wrapper function that implements get_next for
// EngineIteratorFromCallable.
template <typename Callable>
static auto GetNextFromCallable(Callable *callable) -> decltype(std::declval<Callable>()()) {
	return callable->operator()();
}

// Wraps a callable object (e.g. C++11 lambda) as an EngineIterator.
template <typename Callable>
ffi::EngineIterator EngineIteratorFromCallable(Callable &callable) {
	auto *get_next = &GetNextFromCallable<Callable>;
	return {&callable, (const void *(*)(void *))get_next};
};

uintptr_t PredicateVisitor::VisitPredicate(PredicateVisitor *predicate, ffi::KernelExpressionVisitorState *state) {
	auto &filters = predicate->column_filters;

	auto it = filters.begin();
	auto end = filters.end();
	auto get_next = [predicate, state, &it, &end]() -> uintptr_t {
		if (it == end) {
			return 0;
		}
		auto &filter = *it++;
		return predicate->VisitFilter(filter.first, *filter.second, state);
	};
	auto eit = EngineIteratorFromCallable(get_next);

	return ffi::visit_predicate_and(state, &eit);
}

uintptr_t PredicateVisitor::VisitConstantFilter(const string &col_name, const ConstantFilter &filter,
                                                ffi::KernelExpressionVisitorState *state) {
	auto maybe_left =
	    ffi::visit_expression_column(state, KernelUtils::ToDeltaString(col_name), DuckDBEngineError::AllocateError);

	uintptr_t left;
	auto left_res = KernelUtils::TryUnpackResult(maybe_left, left);
	if (left_res.HasError()) {
		error_data = left_res;
		return ~0;
	}

	uintptr_t right = ~0;
	auto &value = filter.constant;
	switch (value.type().id()) {
	case LogicalType::BIGINT:
		right = visit_expression_literal_long(state, BigIntValue::Get(value));
		break;
	case LogicalType::INTEGER:
		right = visit_expression_literal_int(state, IntegerValue::Get(value));
		break;
	case LogicalType::SMALLINT:
		right = visit_expression_literal_short(state, SmallIntValue::Get(value));
		break;
	case LogicalType::TINYINT:
		right = visit_expression_literal_byte(state, TinyIntValue::Get(value));
		break;
	case LogicalType::FLOAT:
		right = visit_expression_literal_float(state, FloatValue::Get(value));
		break;
	case LogicalType::DOUBLE:
		right = visit_expression_literal_double(state, DoubleValue::Get(value));
		break;
	case LogicalType::BOOLEAN:
		right = visit_expression_literal_bool(state, BooleanValue::Get(value));
		break;
	case LogicalTypeId::DATE:
	    right = visit_expression_literal_date(state, DateValue::Get(value).days);
	    break;
	case LogicalType::VARCHAR: {
		// WARNING: C++ lifetime extension rules don't protect calls of the form
		// foo(std::string(...).c_str())
		auto str = StringValue::Get(value);
		auto maybe_right = ffi::visit_expression_literal_string(state, KernelUtils::ToDeltaString(str),
		                                                        DuckDBEngineError::AllocateError);
		auto right_res = KernelUtils::TryUnpackResult(maybe_right, right);
		if (right_res.HasError()) {
			error_data = right_res;
			return ~0;
		}
		break;
	}
	// TODO: implement these types
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::DECIMAL:
	default:
		break; // unsupported type
	}

	// TODO support other comparison types?
	switch (filter.comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
		return visit_predicate_lt(state, left, right);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return visit_predicate_le(state, left, right);
	case ExpressionType::COMPARE_GREATERTHAN:
		return visit_predicate_gt(state, left, right);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return visit_predicate_ge(state, left, right);
	case ExpressionType::COMPARE_EQUAL:
		return visit_predicate_eq(state, left, right);
    case ExpressionType::COMPARE_NOTEQUAL:
	    return ffi::visit_predicate_ne(state, left, right);
	// TODO: evaluate for implementation
	case ExpressionType::COMPARE_BETWEEN:
	case ExpressionType::COMPARE_NOT_BETWEEN:
	case ExpressionType::COMPARE_NOT_IN:
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
	default:
		// TODO: add more types
		return ~0; // Unsupported operation
	}
}

uintptr_t PredicateVisitor::VisitAndFilter(const string &col_name, const ConjunctionAndFilter &filter,
                                           ffi::KernelExpressionVisitorState *state) {
	auto it = filter.child_filters.begin();
	auto end = filter.child_filters.end();
	auto get_next = [this, col_name, state, &it, &end]() -> uintptr_t {
		if (it == end) {
			return 0;
		}
		auto &child_filter = *it++;

		return VisitFilter(col_name, *child_filter, state);
	};
	auto eit = EngineIteratorFromCallable(get_next);
	return visit_predicate_and(state, &eit);
}

uintptr_t PredicateVisitor::VisitIsNull(const string &col_name, ffi::KernelExpressionVisitorState *state) {
	auto maybe_inner =
	    ffi::visit_expression_column(state, KernelUtils::ToDeltaString(col_name), DuckDBEngineError::AllocateError);
	uintptr_t inner;

	auto err = KernelUtils::TryUnpackResult(maybe_inner, inner);
	if (err.HasError()) {
		error_data = err;
		return ~0;
	}
	return ffi::visit_predicate_is_null(state, inner);
}

uintptr_t PredicateVisitor::VisitIsNotNull(const string &col_name, ffi::KernelExpressionVisitorState *state) {
	return ffi::visit_predicate_not(state, VisitIsNull(col_name, state));
}

uintptr_t PredicateVisitor::VisitFilter(const string &col_name, const TableFilter &filter,
                                        ffi::KernelExpressionVisitorState *state) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON:
		return VisitConstantFilter(col_name, static_cast<const ConstantFilter &>(filter), state);
	case TableFilterType::CONJUNCTION_AND:
		return VisitAndFilter(col_name, static_cast<const ConjunctionAndFilter &>(filter), state);
	case TableFilterType::IS_NULL:
		return VisitIsNull(col_name, state);
	case TableFilterType::IS_NOT_NULL:
		return VisitIsNotNull(col_name, state);
	// TODO: implement once kernel can do arbitrary expressions
	case TableFilterType::EXPRESSION_FILTER:
    // TODO: implement once kernel adds support for IN filters / arbitrary expressions
	case TableFilterType::IN_FILTER:
    // TODO: implement once kernel can do struct extract pushdown / arbitrary expressions
    case TableFilterType::STRUCT_EXTRACT:
    // TODO: figure out if this is ever useful
	case TableFilterType::DYNAMIC_FILTER:
	// TODO: can we even push these down?
	case TableFilterType::CONJUNCTION_OR:
	case TableFilterType::OPTIONAL_FILTER:
	default:
		return ~0;
	}
}

void LoggerCallback::Initialize(DatabaseInstance &db_p) {
	auto &instance = GetInstance();
	unique_lock<mutex> lck(instance.lock);
	if (instance.db.expired()) {
		instance.db = db_p.shared_from_this();
	}
}

void LoggerCallback::CallbackEvent(ffi::Event event) {
	auto &instance = GetInstance();
	auto db_locked = instance.db.lock();
	if (db_locked) {
		// Note: this slightly offbeat invocation of logging API is because we are passing through the log level instead
		// of using the same
		//       log level for every message of this log type. We may
		DUCKDB_LOG_INTERNAL(*db_locked, DeltaKernelLogType::NAME, GetDuckDBLogLevel(event.level),
		                    DeltaKernelLogType::ConstructLogMessage(event));
	}
}

LogLevel LoggerCallback::GetDuckDBLogLevel(ffi::Level level) {
	switch (level) {
	case ffi::Level::TRACE:
		return LogLevel::LOG_TRACE;
	case ffi::Level::DEBUGGING:
		return LogLevel::LOG_DEBUG;
	case ffi::Level::INFO:
		return LogLevel::LOG_INFO;
	case ffi::Level::WARNING:
		return LogLevel::LOG_WARNING;
	case ffi::Level::ERROR:
		return LogLevel::LOG_ERROR;
	default:
		throw InternalException("Unknown log level");
	}
}

LoggerCallback &LoggerCallback::GetInstance() {
	static LoggerCallback instance;
	return instance;
}

void LoggerCallback::DuckDBSettingCallBack(ClientContext &context, SetScope scope, Value &parameter) {
	Value current_setting;
	auto res = context.TryGetCurrentSetting("delta_kernel_logging", current_setting);

	if (res.GetScope() == SettingScope::INVALID) {
		throw InternalException("Failed to find setting 'delta_kernel_logging'");
	}

	if (current_setting.GetValue<bool>() && !parameter.GetValue<bool>()) {
		throw InvalidInputException("Can not disable 'delta_kernel_logging' after enabling it. You can disable DuckDB "
		                            "logging with SET enable_logging=false, but there will still be some performance "
		                            "overhead from 'delta_kernel_logging'"
		                            "that can only be mitigated by restarting DuckDB");
	}

	if (!current_setting.GetValue<bool>() && parameter.GetValue<bool>()) {
		ffi::enable_event_tracing(LoggerCallback::CallbackEvent, ffi::Level::TRACE);
	}
}

}; // namespace duckdb
