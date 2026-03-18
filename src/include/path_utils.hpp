//===----------------------------------------------------------------------===//
//                         DuckDB
//
// path_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/path.hpp"

//
// Local Path utilities for the Delta extension — thin wrappers over duckdb::Path
// for methods not yet in DuckDB core (ToLocal, ToFileLocal, GetCommonLineage).
// TODO: (@benfleis) after these land in duckdb core, delete this file and update callers.
//

namespace duckdb {

// file:/{1,3}... -> /..., throws if non-local
Path PathToLocal(const Path &path);

// /... -> file:///..., throws if non-local
Path PathToFileLocal(const Path &path);

// Returns depth (>=0) if `ancestor` is a prefix of `path`, -1 if not.
// min_depth=-1: any depth; min_depth>=0: only match if depth >= min_depth.
int PathGetCommonLineage(const Path &path, const Path &ancestor, int min_depth = -1);

} // namespace duckdb
