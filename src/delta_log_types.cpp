#include "delta_log_types.hpp"
#include "delta_utils.hpp"

namespace duckdb {

constexpr LogLevel DeltaKernelLogType::LEVEL;

DeltaKernelLogType::DeltaKernelLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType DeltaKernelLogType::GetLogType() {
	child_list_t<LogicalType> request_child_list = {{"target", LogicalType::VARCHAR},
	                                                {"message", LogicalType::VARCHAR},
	                                                {"file", LogicalType::VARCHAR},
	                                                {"line", LogicalType::UINTEGER}};
	return LogicalType::STRUCT(request_child_list);
}

string DeltaKernelLogType::ConstructLogMessage(ffi::Event event) {
	Value file, line;

	auto file_string = KernelUtils::FromDeltaString(event.file);
	if (!file_string.empty()) {
		file = Value(KernelUtils::FromDeltaString(event.file));
		line = Value::UINTEGER(event.line);
	}

	child_list_t<Value> message_child_list = {{"target", Value(KernelUtils::FromDeltaString(event.target))},
	                                          {"message", Value(KernelUtils::FromDeltaString(event.message))},
	                                          {"file", Value(file)},
	                                          {"line", Value(line)}};

	return Value::STRUCT(message_child_list).ToString();
}

}; // namespace duckdb
