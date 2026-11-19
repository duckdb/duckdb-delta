//===----------------------------------------------------------------------===//
//                         DuckDB
//
// delta_interval.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {
class ExtensionLoader;
class ClientContext;
class LogicalOperator;

//! Spark has two interval types (written to Delta tables with its type check off; the Delta protocol defines none) and
//! DuckDB one. A Delta interval column reads as DuckDB INTERVAL under an alias that keeps its kind, so DuckDB can
//! refuse what Spark refuses: comparing, combining or adding the two kinds, and values of the other kind. Casting to
//! plain INTERVAL opts into DuckDB's INTERVAL semantics.
struct DeltaInterval {
	//! `interval year to month`: a count of months; stored as {months, 0, 0}
	static constexpr const char *YEAR_MONTH_NAME = "INTERVAL_YEAR_MONTH";
	//! `interval day to second`: a count of microseconds; stored as {0, 0, micros}, as DuckDB imports a duration
	static constexpr const char *DAY_TIME_NAME = "INTERVAL_DAY_TIME";

	static LogicalType YearMonth();
	static LogicalType DayTime();
	static bool IsYearMonth(const LogicalType &type);
	static bool IsDayTime(const LogicalType &type);
	static bool IsDeltaInterval(const LogicalType &type);
	//! The type named `name` (case-insensitive), or INVALID
	static LogicalType FromName(const string &name);
	//! `type` with each interval kind replaced by its parquet storage type (INTEGER months, BIGINT micros)
	static LogicalType ToStorageType(const LogicalType &type);
	//! The value of `type` stored as `storage` (INTEGER months or BIGINT micros), as kernel hands a literal
	static bool TryFromStorage(const Value &storage, const LogicalType &type, Value &result);

	//! Registers the two types, their casts and the rules that keep the kinds apart
	static void Register(ExtensionLoader &loader);
	//! The types mean something only to this extension, so a DuckDB table (or type) outside a Delta catalog may not
	//! store them: opened without delta loaded, its columns would not compare, sort or cast
	static void RejectOutsideDelta(ClientContext &context, LogicalOperator &op);
	//! A window RANGE offset over an ORDER BY key of a kind must be a plain INTERVAL constant that fits the kind: core
	//! binds the frame bound after the key's INTERVAL collation, so a plain offset reaches only core's INTERVAL
	//! arithmetic, and a typed one frames the collated key in the kind
	static void CheckWindowRanges(ClientContext &context, LogicalOperator &op);
};

} // namespace duckdb
