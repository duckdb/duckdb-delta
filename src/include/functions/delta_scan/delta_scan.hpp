//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_multi_file_list.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class DeltaFilterPushdownMode : uint8_t {
	NONE = 0,
	ALL = 1,
	CONSTANT_ONLY = 2,
	DYNAMIC_ONLY = 3,
};

static constexpr DeltaFilterPushdownMode DEFAULT_PUSHDOWN_MODE = DeltaFilterPushdownMode::ALL;

struct DeltaEnumUtils {
	static DeltaFilterPushdownMode FromString(const string &str);
	static string ToString(const DeltaFilterPushdownMode &mode);
};

struct DeltaFunctionInfo : public TableFunctionInfo {
	shared_ptr<DeltaMultiFileList> snapshot;
	string expected_path;
	string table_name;
};

} // namespace duckdb
