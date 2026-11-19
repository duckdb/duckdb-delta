#include "delta_interval.hpp"

#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/function/combine_types_rule.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/planner/operator/logical_alter.hpp"
#include "duckdb/planner/operator/logical_create.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

LogicalType DeltaInterval::YearMonth() {
	return LogicalType(LogicalTypeId::INTERVAL).WithAlias(YEAR_MONTH_NAME);
}

LogicalType DeltaInterval::DayTime() {
	return LogicalType(LogicalTypeId::INTERVAL).WithAlias(DAY_TIME_NAME);
}

bool DeltaInterval::IsYearMonth(const LogicalType &type) {
	return type.id() == LogicalTypeId::INTERVAL && type.GetAlias() == YEAR_MONTH_NAME;
}

bool DeltaInterval::IsDayTime(const LogicalType &type) {
	return type.id() == LogicalTypeId::INTERVAL && type.GetAlias() == DAY_TIME_NAME;
}

bool DeltaInterval::IsDeltaInterval(const LogicalType &type) {
	return IsYearMonth(type) || IsDayTime(type);
}

LogicalType DeltaInterval::ToStorageType(const LogicalType &type) {
	return TypeVisitor::VisitReplace(type, [](const LogicalType &child) {
		if (IsYearMonth(child)) {
			return LogicalType(LogicalType::INTEGER);
		}
		if (IsDayTime(child)) {
			return LogicalType(LogicalType::BIGINT);
		}
		return child;
	});
}

LogicalType DeltaInterval::FromName(const string &name) {
	if (StringUtil::CIEquals(name, YEAR_MONTH_NAME)) {
		return YearMonth();
	}
	if (StringUtil::CIEquals(name, DAY_TIME_NAME)) {
		return DayTime();
	}
	return LogicalType::INVALID;
}

bool DeltaInterval::TryFromStorage(const Value &storage, const LogicalType &type, Value &result) {
	if (storage.IsNull()) {
		result = Value(type);
		return true;
	}
	if (IsYearMonth(type) && storage.type().id() == LogicalTypeId::INTEGER) {
		result = Value::INTERVAL(storage.GetValue<int32_t>(), 0, 0).WithType(type);
		return true;
	}
	if (IsDayTime(type) && storage.type().id() == LogicalTypeId::BIGINT) {
		result = Value::INTERVAL(0, 0, storage.GetValue<int64_t>()).WithType(type);
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Values
//===--------------------------------------------------------------------===//

//! A year-month value holds months only
static bool TryToYearMonth(interval_t input, interval_t &result, CastParameters &parameters) {
	if (input.days != 0 || input.micros != 0) {
		HandleCastError::AssignError(
		    StringUtil::Format("Cannot cast INTERVAL '%s' to %s: a Delta year-month interval holds whole months only",
		                       Interval::ToString(input), DeltaInterval::YEAR_MONTH_NAME),
		    parameters);
		return false;
	}
	result = input;
	return true;
}

//! A day-time value holds an exact duration, kept in micros as DuckDB keeps an imported duration
static bool TryToDayTime(interval_t input, interval_t &result, CastParameters &parameters) {
	// days and micros may have opposite signs, so only the sum has to fit (`106751992 days -24 hours` does)
	auto total = hugeint_t(input.days) * hugeint_t(Interval::MICROS_PER_DAY) + hugeint_t(input.micros);
	if (input.months != 0 || !Hugeint::TryCast<int64_t>(total, result.micros)) {
		auto reason = input.months != 0 ? "a Delta day-time interval holds no months"
		                                : "it is out of range for a Delta day-time interval";
		HandleCastError::AssignError(StringUtil::Format("Cannot cast INTERVAL '%s' to %s: %s",
		                                                Interval::ToString(input), DeltaInterval::DAY_TIME_NAME,
		                                                reason),
		                             parameters);
		return false;
	}
	result.months = 0;
	result.days = 0;
	return true;
}

struct ToYearMonthOperator {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST &result, CastParameters &parameters) {
		return TryToYearMonth(input, result, parameters);
	}
};

struct ToDayTimeOperator {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST &result, CastParameters &parameters) {
		return TryToDayTime(input, result, parameters);
	}
};

//===--------------------------------------------------------------------===//
// Casts
//===--------------------------------------------------------------------===//

//! Spark writes a year-month value as INT32 months, a day-time value as INT64 micros (no parquet annotation)
static bool MonthsToYearMonth(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<int32_t, interval_t>(source, result, count, [](int32_t months) {
		interval_t value;
		value.months = months;
		value.days = 0;
		value.micros = 0;
		return value;
	});
	return true;
}

static bool MicrosToDayTime(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<int64_t, interval_t>(source, result, count, [](int64_t micros) {
		interval_t value;
		value.months = 0;
		value.days = 0;
		value.micros = micros;
		return value;
	});
	return true;
}

//! VARCHAR goes through DuckDB's INTERVAL parsing, then the kind check
struct ParseThenCheckData : public BoundCastData {
	ParseThenCheckData(BoundCastInfo parse_p, cast_function_t check_p) : parse(std::move(parse_p)), check(check_p) {
	}
	BoundCastInfo parse;
	cast_function_t check;

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<ParseThenCheckData>(parse.Copy(), check);
	}
};

static bool ParseThenCheck(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &data = parameters.cast_data->Cast<ParseThenCheckData>();
	Vector parsed(LogicalType::INTERVAL, count);
	CastParameters parse_parameters(parameters, data.parse.GetCastData(), parameters.local_state);
	bool all_ok = data.parse.Cast(source, parsed, count, parse_parameters);
	all_ok = data.check(parsed, result, count, parameters) && all_ok;
	return all_ok;
}

template <cast_function_t CHECK>
static BoundCastInfo BindParseThenCheck(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	auto parse = input.GetCastFunction(source, LogicalType::INTERVAL);
	return BoundCastInfo(ParseThenCheck, make_uniq<ParseThenCheckData>(std::move(parse), CHECK));
}

//! A kind is not a count, whatever its storage: `ym::INTEGER` and `ym = 14` are refused
static BoundCastInfo BindRefuseToNumber(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	throw ConversionException("Cannot cast %s to %s: a Delta interval is not a number", source.ToString(),
	                          target.ToString());
}

static BoundCastInfo BindRefuseOtherKind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	throw ConversionException("Cannot cast %s to %s: Delta keeps year-month and day-time intervals apart. Cast to "
	                          "INTERVAL first to use DuckDB's INTERVAL semantics",
	                          source.ToString(), target.ToString());
}

//===--------------------------------------------------------------------===//
// Combining types
//===--------------------------------------------------------------------===//

//! A number next to a kind (`ym = 14`): core would prefer the aliased side and cast the number into it, so `14` would
//! count months and `86400000000` micros. Spark and DuckDB refuse INTERVAL = 14. Here the pair resolves to the
//! number, and the kind -> number cast refuses at bind.
static bool IsNumber(const LogicalType &type) {
	return type.IsNumeric() || type.id() == LogicalTypeId::INTEGER_LITERAL;
}

static bool MatchesDeltaInterval(const LogicalType &left, const LogicalType &right) {
	auto left_kind = DeltaInterval::IsDeltaInterval(left);
	auto right_kind = DeltaInterval::IsDeltaInterval(right);
	if (!left_kind && !right_kind) {
		return false;
	}
	if ((left_kind && IsNumber(right)) || (right_kind && IsNumber(left))) {
		return true;
	}
	return left.id() == LogicalTypeId::INTERVAL && right.id() == LogicalTypeId::INTERVAL;
}

//! The two kinds do not combine (comparison, UNION, CASE, IN, ...), as in Spark; a plain INTERVAL combines into the
//! Delta kind and is checked by the cast, so `ym = INTERVAL '30 days'` fails instead of comparing a month as 30 days.
//! A number does not combine with a kind.
static bool CombineDeltaInterval(LogicalTypeResolver &resolver, const LogicalType &left, const LogicalType &right,
                                 LogicalType &result) {
	if (IsNumber(left) || IsNumber(right)) {
		auto &number = IsNumber(left) ? left : right;
		result = number.id() == LogicalTypeId::INTEGER_LITERAL ? IntegerLiteral::GetType(number) : number;
		return true;
	}
	if (left == right) {
		result = left;
		return true;
	}
	if (!left.HasAlias()) {
		result = right;
		return true;
	}
	if (!right.HasAlias()) {
		result = left;
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Arithmetic
//===--------------------------------------------------------------------===//

//! Without these overloads `ym + dt` binds to INTERVAL + INTERVAL through the implicit casts and mixes the kinds
//! (`'1 month' - '30 days'` = 0). Exact overloads win over the casts: a kind plus the same kind, or plus a plain
//! INTERVAL that fits it (checked as in a comparison), stays that kind; the two kinds together are refused at bind.
static unique_ptr<FunctionData> BindRefuseMixedArithmetic(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	throw BinderException("Cannot apply %s to %s and %s: Delta keeps year-month and day-time intervals apart. Cast "
	                      "to INTERVAL first to use DuckDB's INTERVAL semantics",
	                      input.GetBoundFunction().GetName(), arguments[0]->GetReturnType().ToString(),
	                      arguments[1]->GetReturnType().ToString());
}

static void UnreachableMixedArithmetic(DataChunk &args, ExpressionState &state, Vector &result) {
	throw InternalException("Delta interval arithmetic across kinds should have been refused at bind");
}

//! The plain INTERVAL operand (if any) must fit the result's kind, as `ym = INTERVAL '30 days'` must
static interval_t CheckFitsKind(interval_t value, const LogicalType &kind) {
	interval_t result;
	string error;
	CastParameters parameters(false, &error);
	auto fits = DeltaInterval::IsYearMonth(kind) ? TryToYearMonth(value, result, parameters)
	                                             : TryToDayTime(value, result, parameters);
	if (!fits) {
		throw ConversionException(error);
	}
	return result;
}

template <class OP, idx_t PLAIN>
static void KindArithmetic(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &kind = result.GetType();
	BinaryExecutor::Execute<interval_t, interval_t, interval_t>(
	    args.data[0], args.data[1], result, args.size(), [&](interval_t left, interval_t right) {
		    if (PLAIN == 0) {
			    left = CheckFitsKind(left, kind);
		    } else if (PLAIN == 1) {
			    right = CheckFitsKind(right, kind);
		    }
		    return OP::template Operation<interval_t, interval_t, interval_t>(left, right);
	    });
}

static void NegateKind(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<interval_t, interval_t>(args.data[0], result, args.size(),
	                                               [](interval_t value) { return Interval::Invert(value); });
}

//! `op` and its alias (`add`, `subtract`) are separate catalog entries, so both get the overloads
template <class OP>
static void AddArithmetic(ExtensionLoader &loader, const string &op, bool with_negate) {
	ScalarFunctionSet set {Identifier(op)};
	if (with_negate) {
		// `-ym` stays year-month, so `-ym = INTERVAL '-420 days'` is refused like `ym = INTERVAL '420 days'`
		for (auto &kind : {DeltaInterval::YearMonth(), DeltaInterval::DayTime()}) {
			ScalarFunction negate({kind}, kind, NegateKind);
			negate.SetFallible();
			set.AddFunction(negate);
		}
	}
	for (auto &kind : {DeltaInterval::YearMonth(), DeltaInterval::DayTime()}) {
		// fallible: the plain operand may not fit the kind, and the sum may overflow
		ScalarFunction same({kind, kind}, kind, KindArithmetic<OP, 2>);
		ScalarFunction plain_right({kind, LogicalType::INTERVAL}, kind, KindArithmetic<OP, 1>);
		ScalarFunction plain_left({LogicalType::INTERVAL, kind}, kind, KindArithmetic<OP, 0>);
		for (auto function : {&same, &plain_right, &plain_left}) {
			function->SetFallible();
			set.AddFunction(*function);
		}
	}
	for (auto &args : {vector<LogicalType> {DeltaInterval::YearMonth(), DeltaInterval::DayTime()},
	                   vector<LogicalType> {DeltaInterval::DayTime(), DeltaInterval::YearMonth()}}) {
		ScalarFunction refused(args, LogicalType::INTERVAL, UnreachableMixedArithmetic, BindRefuseMixedArithmetic);
		// a typed NULL operand would otherwise fold to NULL before the bind callback refuses the pair
		refused.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		set.AddFunction(refused);
	}
	loader.AddFunctionOverload(std::move(set));
}

//===--------------------------------------------------------------------===//
// Storage outside Delta
//===--------------------------------------------------------------------===//

static bool ContainsDeltaInterval(const LogicalType &type) {
	return TypeVisitor::Contains(type, [](const LogicalType &child) { return DeltaInterval::IsDeltaInterval(child); });
}

static bool IsDeltaCatalog(Catalog &catalog) {
	return catalog.GetCatalogType() == "delta";
}

static void ThrowStoredOutsideDelta(const string &name, const LogicalType &type) {
	throw NotImplementedException("Cannot store %s of type %s outside a Delta table: the type exists only while the "
	                              "delta extension is loaded. Cast it to INTERVAL to store it in a DuckDB table.",
	                              name, type.ToString());
}

static void ThrowIfColumnsHoldKind(const ColumnList &columns) {
	for (auto &column : columns.Logical()) {
		if (ContainsDeltaInterval(column.Type())) {
			ThrowStoredOutsideDelta(StringUtil::Format("column '%s'", column.Name().GetIdentifierName()),
			                        column.Type());
		}
	}
}

void DeltaInterval::RejectOutsideDelta(ClientContext &context, LogicalOperator &op) {
	// EXPLAIN ANALYZE runs the DDL in its child plan
	for (auto &child : op.children) {
		RejectOutsideDelta(context, *child);
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_CREATE_TABLE: {
		auto &create = op.Cast<LogicalCreateTable>();
		if (!IsDeltaCatalog(create.schema.ParentCatalog())) {
			ThrowIfColumnsHoldKind(create.info->Base().columns);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_CREATE_TYPE: {
		auto &create = op.Cast<LogicalCreate>();
		if (create.schema && IsDeltaCatalog(create.schema->ParentCatalog())) {
			return;
		}
		auto &info = create.info->Cast<CreateTypeInfo>();
		if (ContainsDeltaInterval(info.type)) {
			ThrowStoredOutsideDelta(StringUtil::Format("type '%s'", info.GetQualifiedName().Name().GetIdentifierName()),
			                        info.type);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_ALTER: {
		auto &alter_info = *op.Cast<LogicalAlter>().info;
		if (alter_info.type != AlterType::ALTER_TABLE) {
			return;
		}
		auto catalog = Catalog::GetCatalogEntry(context, alter_info.GetQualifiedName().Catalog());
		if (!catalog || IsDeltaCatalog(*catalog)) {
			return;
		}
		auto &table_info = alter_info.Cast<AlterTableInfo>();
		switch (table_info.alter_table_type) {
		case AlterTableType::ADD_COLUMN: {
			auto &column = table_info.Cast<AddColumnInfo>().new_column;
			if (ContainsDeltaInterval(column.Type())) {
				ThrowStoredOutsideDelta(StringUtil::Format("column '%s'", column.Name().GetIdentifierName()),
				                        column.Type());
			}
			break;
		}
		case AlterTableType::ADD_FIELD: {
			auto &field = table_info.Cast<AddFieldInfo>().new_field;
			if (ContainsDeltaInterval(field.Type())) {
				ThrowStoredOutsideDelta(StringUtil::Format("field '%s'", field.Name().GetIdentifierName()),
				                        field.Type());
			}
			break;
		}
		case AlterTableType::ALTER_COLUMN_TYPE: {
			auto &change = table_info.Cast<ChangeColumnTypeInfo>();
			if (ContainsDeltaInterval(change.target_type)) {
				auto name = change.column_path.empty()
				                ? string("a column")
				                : StringUtil::Format("column '%s'", change.column_path.back().GetIdentifierName());
				ThrowStoredOutsideDelta(name, change.target_type);
			}
			break;
		}
		default:
			break;
		}
		return;
	}
	default:
		return;
	}
}

//===--------------------------------------------------------------------===//
// Window RANGE frames
//===--------------------------------------------------------------------===//

static bool IsRangeBoundary(WindowBoundary boundary) {
	return boundary == WindowBoundary::EXPR_PRECEDING_RANGE || boundary == WindowBoundary::EXPR_FOLLOWING_RANGE;
}

//! The kind of an ORDER BY key, seen through core's INTERVAL collation (`normalized_interval(CAST(ym AS INTERVAL))`):
//! only a key that is itself a kind counts, not one computed from a kind (`ts + dt`, `epoch_us(ym)`, `ym * 1.5`). An
//! explicit `ORDER BY ym::INTERVAL` gives the same tree as the collation, so it counts too; casting the key in a
//! subquery is the way to DuckDB's rules (the window then sees a plain column)
static LogicalType FindKeyKind(const Expression &key) {
	reference<const Expression> current(key);
	while (true) {
		auto &expr = current.get();
		if (DeltaInterval::IsDeltaInterval(expr.GetReturnType())) {
			return expr.GetReturnType();
		}
		if (expr.GetReturnType().id() != LogicalTypeId::INTERVAL) {
			return LogicalType::INVALID;
		}
		if (BoundCastExpression::IsCast(expr)) {
			current = BoundCastExpression::Child(expr.Cast<BoundFunctionExpression>());
			continue;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &function = expr.Cast<BoundFunctionExpression>();
			if (function.Function().GetName() == "normalized_interval" && function.GetChildren().size() == 1) {
				current = *function.GetChildren()[0];
				continue;
			}
		}
		return LogicalType::INVALID;
	}
}

static constexpr const char *RANGE_HINT =
    "To use DuckDB's INTERVAL rules, cast the key to INTERVAL in a subquery and order the window by that column";

//! A bound RANGE frame edge is `key -/+ offset`, bound by core as a scalar function call, possibly under a cast when
//! the other edge's offset gave the frame another type
static void CheckRangeEdge(ClientContext &context, const LogicalType &kind, const unique_ptr<Expression> &edge_p) {
	if (!edge_p) {
		return;
	}
	reference<const Expression> edge(*edge_p);
	while (BoundCastExpression::IsCast(edge.get())) {
		edge = BoundCastExpression::Child(edge.get().Cast<BoundFunctionExpression>());
	}
	// a NULL offset folds `key - NULL` to a NULL constant before this runs; the key is not a constant, so a constant
	// edge means a NULL offset. Core would frame peers only (or, with a typed day-time NULL, fail on the collated key)
	if (edge.get().GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		throw BinderException("Window RANGE offset over an ORDER BY key of type %s: the offset must not be NULL",
		                      kind.ToString());
	}
	if (edge.get().GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return;
	}
	auto &children = edge.get().Cast<BoundFunctionExpression>().GetChildren();
	if (children.size() != 2) {
		return;
	}
	auto &offset = *children[1];
	// a typed offset makes core frame the key in that type, after the collation folded days into months: refused
	// whatever its kind, so the check below sees every offset as a plain INTERVAL
	if (DeltaInterval::IsDeltaInterval(offset.GetReturnType())) {
		throw BinderException("Window RANGE offset over an ORDER BY key of type %s: the offset has type %s; give it "
		                      "as a plain INTERVAL constant or a string, which is checked against the key's kind. %s",
		                      kind.ToString(), offset.GetReturnType().ToString(), RANGE_HINT);
	}
	if (!offset.IsFoldable()) {
		throw BinderException("Window RANGE offset over an ORDER BY key of type %s: the offset must be a constant "
		                      "(not a column or a parameter), so that it can be checked against the key's kind. %s",
		                      kind.ToString(), RANGE_HINT);
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, offset);
	if (value.IsNull() || value.type().id() != LogicalTypeId::INTERVAL) {
		return;
	}
	interval_t fitted;
	string error;
	CastParameters parameters(false, &error);
	auto input = value.GetValue<interval_t>();
	auto fits = DeltaInterval::IsYearMonth(kind) ? TryToYearMonth(input, fitted, parameters)
	                                             : TryToDayTime(input, fitted, parameters);
	if (!fits) {
		throw BinderException("Window RANGE offset over an ORDER BY key of type %s: %s. %s", kind.ToString(), error,
		                      RANGE_HINT);
	}
}

static void CheckWindowRangesInExpression(ClientContext &context, Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_WINDOW) {
		auto &window = expr.Cast<BoundWindowExpression>();
		// the kind comes from the window's own ORDER BY, which a folded frame edge no longer shows
		auto kind = window.OrderBy().size() == 1 ? FindKeyKind(*window.OrderBy()[0].expression) : LogicalType::INVALID;
		if (kind.id() != LogicalTypeId::INVALID) {
			if (IsRangeBoundary(window.WindowStart())) {
				CheckRangeEdge(context, kind, window.StartExpr());
			}
			if (IsRangeBoundary(window.WindowEnd())) {
				CheckRangeEdge(context, kind, window.EndExpr());
			}
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subquery = expr.Cast<BoundSubqueryExpression>();
		if (subquery.Subquery().plan) {
			DeltaInterval::CheckWindowRanges(context, *subquery.Subquery().plan);
		}
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](Expression &child) { CheckWindowRangesInExpression(context, child); });
}

void DeltaInterval::CheckWindowRanges(ClientContext &context, LogicalOperator &op) {
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expression) { CheckWindowRangesInExpression(context, **expression); });
	for (auto &child : op.children) {
		CheckWindowRanges(context, *child);
	}
}

void DeltaInterval::Register(ExtensionLoader &loader) {
	auto year_month = YearMonth();
	auto day_time = DayTime();
	loader.RegisterType(YEAR_MONTH_NAME, year_month);
	loader.RegisterType(DAY_TIME_NAME, day_time);

	loader.RegisterCastFunction(LogicalType::INTEGER, year_month, MonthsToYearMonth);
	loader.RegisterCastFunction(LogicalType::BIGINT, day_time, MicrosToDayTime);

	loader.RegisterCastFunction(LogicalType::INTERVAL, year_month,
	                            VectorCastHelpers::TryCastErrorLoop<interval_t, interval_t, ToYearMonthOperator>);
	loader.RegisterCastFunction(LogicalType::INTERVAL, day_time,
	                            VectorCastHelpers::TryCastErrorLoop<interval_t, interval_t, ToDayTimeOperator>);
	loader.RegisterCastFunction(
	    LogicalType::VARCHAR, year_month,
	    BindParseThenCheck<VectorCastHelpers::TryCastErrorLoop<interval_t, interval_t, ToYearMonthOperator>>);
	loader.RegisterCastFunction(
	    LogicalType::VARCHAR, day_time,
	    BindParseThenCheck<VectorCastHelpers::TryCastErrorLoop<interval_t, interval_t, ToDayTimeOperator>>);

	// Either kind is an INTERVAL value, so functions over INTERVAL (`ts + ym`, epoch_us, avg, ...) take it implicitly
	loader.RegisterCastFunction(year_month, LogicalType::INTERVAL, DefaultCasts::ReinterpretCast, 1);
	loader.RegisterCastFunction(day_time, LogicalType::INTERVAL, DefaultCasts::ReinterpretCast, 1);

	vector<LogicalType> numbers {LogicalType::TINYINT,
	                             LogicalType::SMALLINT,
	                             LogicalType::INTEGER,
	                             LogicalType::BIGINT,
	                             LogicalType::HUGEINT,
	                             LogicalType::UTINYINT,
	                             LogicalType::USMALLINT,
	                             LogicalType::UINTEGER,
	                             LogicalType::UBIGINT,
	                             LogicalType::UHUGEINT,
	                             LogicalType::FLOAT,
	                             LogicalType::DOUBLE,
	                             LogicalType(LogicalTypeId::DECIMAL)};
	for (auto &number : numbers) {
		loader.RegisterCastFunction(year_month, number, BindRefuseToNumber);
		loader.RegisterCastFunction(day_time, number, BindRefuseToNumber);
	}
	loader.RegisterCastFunction(year_month, day_time, BindRefuseOtherKind);
	loader.RegisterCastFunction(day_time, year_month, BindRefuseOtherKind);

	loader.RegisterCombineTypesRule(CombineTypesRule {MatchesDeltaInterval, CombineDeltaInterval});

	AddArithmetic<AddOperator>(loader, "+", false);
	AddArithmetic<AddOperator>(loader, "add", false);
	AddArithmetic<SubtractOperator>(loader, "-", true);
	AddArithmetic<SubtractOperator>(loader, "subtract", true);

	// A literal NULL or string next to a kind in `+`/`-` would tie between the kind's overloads (or, for a plain
	// INTERVAL, between the kind's and core's): costed just above core's cast to INTERVAL, they resolve to INTERVAL,
	// which the kind's overload then checks
	auto &db = loader.GetDatabaseInstance();
	auto null_cost = CastFunctionSet::ImplicitCastCost(db, LogicalType::SQLNULL, LogicalType::INTERVAL) + 1;
	auto string_cost =
	    CastFunctionSet::ImplicitCastCost(db, LogicalType(LogicalTypeId::STRING_LITERAL), LogicalType::INTERVAL) + 1;
	for (auto &kind : {year_month, day_time}) {
		loader.RegisterCastFunction(LogicalType::SQLNULL, kind, DefaultCasts::TryVectorNullCast, null_cost);
	}
	loader.RegisterCastFunction(
	    LogicalType(LogicalTypeId::STRING_LITERAL), year_month,
	    BindParseThenCheck<VectorCastHelpers::TryCastErrorLoop<interval_t, interval_t, ToYearMonthOperator>>,
	    string_cost);
	loader.RegisterCastFunction(
	    LogicalType(LogicalTypeId::STRING_LITERAL), day_time,
	    BindParseThenCheck<VectorCastHelpers::TryCastErrorLoop<interval_t, interval_t, ToDayTimeOperator>>,
	    string_cost);
}

} // namespace duckdb
