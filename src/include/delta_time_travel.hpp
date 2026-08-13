//===----------------------------------------------------------------------===//
//                         DuckDB
//
// delta_time_travel.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {
class BoundAtClause;

//! A time travel target as written by the user: nothing (the latest version), a version, or a
//! timestamp that still has to be resolved into one. Once resolved, a timestamp is a version like any
//! other. Exactly one of the three holds at a time, and reading the wrong one is an error rather than
//! a silently wrong value.
class DeltaTimeTravelSpec {
public:
	DeltaTimeTravelSpec() = default;

	static DeltaTimeTravelSpec FromAtClause(const BoundAtClause &at_clause);
	static DeltaTimeTravelSpec FromVersion(idx_t version);
	static DeltaTimeTravelSpec FromTimestamp(timestamp_tz_t timestamp);

	bool IsLatest() const {
		return kind == Kind::LATEST;
	}
	bool IsVersion() const {
		return kind == Kind::VERSION;
	}
	bool IsTimestamp() const {
		return kind == Kind::TIMESTAMP;
	}

	idx_t GetVersion() const;
	timestamp_tz_t GetTimestamp() const;

private:
	enum class Kind : uint8_t { LATEST, VERSION, TIMESTAMP };

	Kind kind = Kind::LATEST;
	idx_t version = DConstants::INVALID_INDEX;
	timestamp_tz_t timestamp = timestamp_tz_t(0);
};

//! Milliseconds since the unix epoch, which is how the delta protocol spells timestamps
int64_t DeltaTimestampToEpochMs(timestamp_tz_t timestamp);

} // namespace duckdb
