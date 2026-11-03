//===----------------------------------------------------------------------===//
//                         DuckDB
//
// delta_table_properties.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! The `delta.*` table property keys duckdb-delta knows, in their canonical (case-sensitive) spelling: kernel's table
//! properties, `delta.feature.<feature>` for each table feature kernel knows, and the protocol versions. The one list
//! CREATE TABLE, CREATE TABLE AS and COPY TO check keys against. Knowing a key does not make it creatable: kernel
//! decides which ones a create may set.
struct DeltaTableProperties {
	static const vector<string> &KnownKeys();

	//! Whether `key` is in the `delta.` namespace, in any case
	static bool IsDeltaKey(const string &key);

	//! The known key that `key` spells, compared case-insensitively
	static optional<string> FindKnownKey(const string &key);

	//! Refuses a `delta.*` key that is not a known key spelled exactly, with a did-you-mean. Other keys pass: they are
	//! properties of the user's own. `statement` names the statement in the error.
	static void ThrowIfUnknownDeltaKey(const string &key, const string &statement);
};

} // namespace duckdb
