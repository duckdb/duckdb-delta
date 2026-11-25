//===----------------------------------------------------------------------===//
//                         DuckDB
//
// delta_schema_builder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"

#include "duckdb/parser/column_list.hpp"

namespace duckdb {

//! DeltaSchemaBuilder is the inverse of SchemaVisitor: it lowers a DuckDB ColumnList into a
//! kernel schema by driving the kernel's visit_field_* entry points. Kernel calls back into
//! Build() while it holds the visitor state, so the object must outlive the FFI call that
//! consumes the ffi::EngineSchema returned by CreateEngineSchema().
class DeltaSchemaBuilder {
public:
	explicit DeltaSchemaBuilder(const ColumnList &columns) : columns(columns) {
	}

	ffi::EngineSchema CreateEngineSchema();

	//! The first type at or below `type` that Delta cannot store, or nullptr: the one list of storable types, which
	//! VisitField checks before it lowers a type and CTAS checks before it writes anything
	static optional_ptr<const LogicalType> FindUnsupportedType(const LogicalType &type);

	//! The most digits a Delta decimal can have. The protocol's limit, and kernel's: its DecimalType takes the
	//! precision as a u8 and refuses more than 38. DuckDB's own DECIMAL can be wider than that.
	static constexpr idx_t MAX_DECIMAL_PRECISION = 38;

	//! The precision of DECIMAL `type` as kernel takes it. Throws for a decimal wider than Delta has, so that a
	//! precision is never narrowed into a different, valid one on its way into kernel's u8.
	static uint8_t DecimalPrecision(const LogicalType &type);

	//! Errors are stashed rather than thrown, since Build() is invoked by kernel across the FFI
	//! boundary. The caller must check this after the consuming FFI call returns.
	const ErrorData &GetError() const {
		return error;
	}

private:
	static uintptr_t Build(void *data, ffi::KernelSchemaVisitorState *state);

	uintptr_t VisitField(ffi::KernelSchemaVisitorState *state, const string &name, const LogicalType &type,
	                     bool nullable);
	uintptr_t Unpack(ffi::ExternResult<uintptr_t> result);

private:
	const ColumnList &columns;
	ErrorData error;
};

} // namespace duckdb
