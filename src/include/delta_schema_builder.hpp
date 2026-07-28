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
