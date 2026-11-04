//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_copy.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/copy_function.hpp"

namespace duckdb {

//! `COPY ... TO 'path' (FORMAT delta)`: writes the query as version 0 of a new Delta table at the path, through the
//! same create transaction and commit as CREATE TABLE AS
struct DeltaCopyFunction {
	static CopyFunction Create();
};

} // namespace duckdb
