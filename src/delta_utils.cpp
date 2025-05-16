#include "delta_utils.hpp"

#include "delta_log_types.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

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
	visitor.visit_le = VisitBinaryExpression<ExpressionType::COMPARE_LESSTHANOREQUALTO, ComparisonExpression>();
	visitor.visit_gt = VisitBinaryExpression<ExpressionType::COMPARE_GREATERTHAN, ComparisonExpression>();
	visitor.visit_ge = VisitBinaryExpression<ExpressionType::COMPARE_GREATERTHANOREQUALTO, ComparisonExpression>();

	visitor.visit_eq = VisitBinaryExpression<ExpressionType::COMPARE_EQUAL, ComparisonExpression>();
	visitor.visit_ne = VisitBinaryExpression<ExpressionType::COMPARE_NOTEQUAL, ComparisonExpression>();
	visitor.visit_distinct = VisitBinaryExpression<ExpressionType::COMPARE_DISTINCT_FROM, ComparisonExpression>();

	visitor.visit_in = VisitVariadicExpression<ExpressionType::COMPARE_IN, OperatorExpression>();
	visitor.visit_not_in = VisitVariadicExpression<ExpressionType::COMPARE_NOT_IN, OperatorExpression>();

	visitor.visit_add = VisitAdditionExpression;
	visitor.visit_minus = VisitSubctractionExpression;
	visitor.visit_multiply = VisitMultiplyExpression;
	visitor.visit_divide = VisitDivideExpression;

	visitor.visit_column = &VisitColumnExpression;
	visitor.visit_struct_expr = &VisitStructExpression;

	visitor.visit_literal_struct = &VisitStructLiteral;

	visitor.visit_not = &VisitNotExpression;
	visitor.visit_is_null = &VisitIsNullExpression;

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

// This function is a workaround for the fact that duckdb disallows using hugeints to store decimals with precision < 18
// whereas kernel does allow this.
static int64_t GetTruncatedDecimalValue(int64_t value_ms, uint64_t value_ls) {
	// First trim msb from lower half
	auto new_value_ls = value_ls << 1;
	new_value_ls = new_value_ls >> 1;

	// Now cast the lower half to signed
	auto lower_cast = UnsafeNumericCast<int64_t>(value_ls);

	// If value_ms was negative, we need to invert
	if (value_ms < 0) {
		lower_cast = -lower_cast;
	}
	return lower_cast;
}

void ExpressionVisitor::VisitDecimalLiteral(void *state, uintptr_t sibling_list_id, int64_t value_ms, uint64_t value_ls,
                                            uint8_t precision, uint8_t scale) {
	try {
		Value decimal_value;
		if (precision < Decimal::MAX_WIDTH_INT64) {
			decimal_value = Value::DECIMAL(GetTruncatedDecimalValue(value_ms, value_ls), precision, scale);
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

ffi::EngineSchemaVisitor SchemaVisitor::CreateSchemaVisitor(SchemaVisitor &state) {
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

	return visitor;
}

unique_ptr<SchemaVisitor::FieldList> SchemaVisitor::VisitSnapshotSchema(ffi::SharedSnapshot *snapshot) {
	SchemaVisitor state;
	auto visitor = CreateSchemaVisitor(state);

	auto schema = logical_schema(snapshot);
	uintptr_t result = visit_schema(schema, &visitor);
	free_schema(schema);

	if (state.error.HasError()) {
		state.error.Throw();
	}

	return state.TakeFieldList(result);
}

unique_ptr<SchemaVisitor::FieldList> SchemaVisitor::VisitSnapshotGlobalReadSchema(ffi::SharedGlobalScanState *state,
                                                                                  bool logical) {
	SchemaVisitor visitor_state;
	auto visitor = CreateSchemaVisitor(visitor_state);

	ffi::Handle<ffi::SharedSchema> schema;
	if (logical) {
		schema = ffi::get_global_logical_schema(state);
	} else {
		schema = ffi::get_global_read_schema(state);
	}

	uintptr_t result = visit_schema(schema, &visitor);
	free_schema(schema);

	if (visitor_state.error.HasError()) {
		visitor_state.error.Throw();
	}

	return visitor_state.TakeFieldList(result);
}

void SchemaVisitor::VisitDecimal(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                                 bool is_nullable, const ffi::CStringMap *metadata, uint8_t precision, uint8_t scale) {
	state->AppendToList(sibling_list_id, name, LogicalType::DECIMAL(precision, scale));
}

uintptr_t SchemaVisitor::MakeFieldList(SchemaVisitor *state, uintptr_t capacity_hint) {
	return state->MakeFieldListImpl(capacity_hint);
}

void SchemaVisitor::VisitStruct(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                                bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id) {
	auto children = state->TakeFieldList(child_list_id);
	state->AppendToList(sibling_list_id, name, LogicalType::STRUCT(std::move(*children)));
}

void SchemaVisitor::VisitArray(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                               bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id) {
	auto children = state->TakeFieldList(child_list_id);

	D_ASSERT(children->size() == 1);
	state->AppendToList(sibling_list_id, name, LogicalType::LIST(children->front().second));
}

void SchemaVisitor::VisitMap(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
                             bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id) {
	auto children = state->TakeFieldList(child_list_id);

	D_ASSERT(children->size() == 2);
	state->AppendToList(sibling_list_id, name, LogicalType::MAP(LogicalType::STRUCT(std::move(*children))));
}

uintptr_t SchemaVisitor::MakeFieldListImpl(uintptr_t capacity_hint) {
	uintptr_t id = next_id++;
	auto list = make_uniq<FieldList>();
	if (capacity_hint > 0) {
		list->reserve(capacity_hint);
	}
	inflight_lists.emplace(id, std::move(list));
	return id;
}

void SchemaVisitor::AppendToList(uintptr_t id, ffi::KernelStringSlice name, LogicalType &&child) {
	auto it = inflight_lists.find(id);
	if (it == inflight_lists.end()) {
		error = ErrorData(ExceptionType::INTERNAL, "Unhandled error in SchemaVisitor::AppendToList");
		return;
	}
	it->second->emplace_back(std::make_pair(string(name.ptr, name.len), std::move(child)));
}

unique_ptr<SchemaVisitor::FieldList> SchemaVisitor::TakeFieldList(uintptr_t id) {
	auto it = inflight_lists.find(id);
	if (it == inflight_lists.end()) {
		error = ErrorData(ExceptionType::INTERNAL, "Unhandled error in SchemaVisitor::TakeFieldList");
		return make_uniq<SchemaVisitor::FieldList>();
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
	                                           "InvalidCommitInfo",
	                                           "FileAlreadyExists",
	                                           "MissingCommitInfo",
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
	return StringUtil::Format("DeltKernel %s (%u): %s", KernelErrorEnumToString(etype_copy), etype_copy, message_copy);
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

vector<unique_ptr<ParsedExpression>> &
KernelUtils::UnpackTopLevelStruct(const vector<unique_ptr<ParsedExpression>> &parsed_expression) {
	if (parsed_expression.size() != 1) {
		throw IOException("Unexpected size of transformation expression returned by delta kernel: %d",
		                  parsed_expression.size());
	}

	const auto &root_expression = parsed_expression.get(0);
	if (root_expression->type != ExpressionType::FUNCTION) {
		throw IOException("Unexpected type of root expression returned by delta kernel: %d", root_expression->type);
	}

	if (root_expression->Cast<FunctionExpression>().function_name != "struct_pack") {
		throw IOException("Unexpected function of root expression returned by delta kernel: %s",
		                  root_expression->Cast<FunctionExpression>().function_name);
	}

	return root_expression->Cast<FunctionExpression>().children;
}

PredicateVisitor::PredicateVisitor(const vector<string> &column_names, optional_ptr<const TableFilterSet> filters) {
	predicate = this;
	visitor = (uintptr_t(*)(void *, ffi::KernelExpressionVisitorState *)) & VisitPredicate;

	if (filters) {
		for (auto &filter : filters->filters) {
			column_filters[column_names[filter.first]] = filter.second.get();
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

	return visit_expression_and(state, &eit);
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
	default:
		break; // unsupported type
	}

	// TODO support other comparison types?
	switch (filter.comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
		return visit_expression_lt(state, left, right);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return visit_expression_le(state, left, right);
	case ExpressionType::COMPARE_GREATERTHAN:
		return visit_expression_gt(state, left, right);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return visit_expression_ge(state, left, right);
	case ExpressionType::COMPARE_EQUAL:
		return visit_expression_eq(state, left, right);

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
	return visit_expression_and(state, &eit);
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
	return ffi::visit_expression_is_null(state, inner);
}

uintptr_t PredicateVisitor::VisitIsNotNull(const string &col_name, ffi::KernelExpressionVisitorState *state) {
	return ffi::visit_expression_not(state, VisitIsNull(col_name, state));
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
	case ffi::Level::WARN:
		return LogLevel::LOG_WARN;
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
