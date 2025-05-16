//===----------------------------------------------------------------------===//
//                         DuckDB
//
// delta_log_types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_kernel_ffi.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class DeltaKernelLogType : public LogType {
public:
	static constexpr const char *NAME = "DeltaKernel";
	static constexpr LogLevel LEVEL =
	    LogLevel::LOG_DEBUG; // WARNING: DeltaKernelLogType is special in that it overrides this base logtype

	//! Construct the log types
	DeltaKernelLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(ffi::Event event);

	// FIXME: HTTPLogType should be structured probably
	static string ConstructLogMessage(const string &str) {
		return str;
	}
};

class DeltaLogTypes {
public:
	static void RegisterLogTypes(DatabaseInstance &instance) {
		instance.GetLogManager().RegisterLogType(make_uniq<DeltaKernelLogType>());
	}
};

} // namespace duckdb
