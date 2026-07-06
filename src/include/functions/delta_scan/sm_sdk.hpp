//===----------------------------------------------------------------------===//
// delta::delta_sdk — DuckDB drives the kernel scan state machine.
//
// The kernel hands us a steppable state machine (ffi::kdf_scan_open); DuckDB owns the loop. We pull
// the next step (kdf_sm_get_step); when the kernel needs a Reduce computed, it gives us SQL
// (kdf_sm_reduce_sql) which WE run in DuckDB, then hand the Arrow result back (kdf_sm_submit_reduce).
// When the SM is Done, kdf_sm_result_sql lowers the terminal ResultPlan to the SQL DuckDB executes.
// No callback is ever passed into the kernel — the kernel is passive, the engine drives it.
//===----------------------------------------------------------------------===//
#pragma once

#include "delta_utils.hpp" // generated_delta_kernel_ffi.hpp (ffi::) + duckdb common

#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace duckdb {
namespace delta_sdk {

//! Run `sql` in DuckDB on `context`'s database and export the entire result as one Arrow C Data
//! batch into the kernel's (ABI-identical) FFI_Arrow* out-params (ownership transfers to the
//! kernel). This is how DuckDB executes a kernel Reduce step. Returns false on query error.
inline bool RunSqlToArrow(ClientContext &context, const string &sql, ffi::FFI_ArrowArray *out_array,
                          ffi::FFI_ArrowSchema *out_schema) {
	// Fresh connection on the same database: a separate transaction independent of the outer bind.
	Connection con(*context.db);
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		return false;
	}
	ClientProperties props = context.GetClientProperties();
	ArrowSchema schema;
	ArrowConverter::ToArrowSchema(&schema, result->types, result->names, props);
	ArrowAppender appender(result->types, STANDARD_VECTOR_SIZE, props, {});
	while (auto chunk = result->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		appender.Append(*chunk, 0, chunk->size(), chunk->size());
	}
	ArrowArray array = appender.Finalize();
	*reinterpret_cast<ArrowSchema *>(out_schema) = schema;
	*reinterpret_cast<ArrowArray *>(out_array) = array;
	return true;
}

[[noreturn]] inline void ThrowFfi(char *err, const char *what) {
	string msg = err ? string(err) : string("unknown error");
	if (err) {
		ffi::kdf_string_free(err);
	}
	throw IOException("delta SM SDK: %s failed: %s", what, msg);
}

//! Drive the kernel scan state machine to completion and return the data-stage DuckDB SQL.
//! `metadata_only` selects the kernel's metadata-only scan SM (file-list terminal) vs the full
//! data+metadata SM. DuckDB owns the loop and executes every Reduce.
//!
//! `predicate`, if non-null, is a data-skipping predicate (an `ffi::EnginePredicate`, e.g. built
//! from a `PredicateVisitor` over the pushed-down `TableFilterSet`) the kernel applies to the scan
//! builder so it emits its stats-based file-skip filter. The kernel visits it ONCE, synchronously,
//! inside `kdf_scan_open` — so the engine state it borrows (the `TableFilterSet`) need only outlive
//! THIS call. When null, no data-skipping predicate is applied.
inline string DriveScan(const string &path, int64_t version, bool metadata_only, ClientContext &context,
                        optional_ptr<ffi::EnginePredicate> predicate = nullptr) {
	char *err = nullptr;
	ffi::KdfSM *sm =
	    ffi::kdf_scan_open(path.c_str(), path.size(), version, metadata_only, predicate.get(), &err);
	if (!sm) {
		ThrowFfi(err, "kdf_scan_open");
	}
	try {
		for (;;) {
			int32_t kind = ffi::kdf_sm_get_step(sm, &err);
			if (kind < 0) {
				ThrowFfi(err, "kdf_sm_get_step");
			}
			if (kind == ffi::KDF_STEP_DONE) {
				break;
			}
			// KDF_STEP_REDUCE: kernel hands us SQL; DuckDB runs it; hand the Arrow result back.
			char *sql_c = ffi::kdf_sm_reduce_sql(sm, &err);
			if (!sql_c) {
				ThrowFfi(err, "kdf_sm_reduce_sql");
			}
			string reduce_sql(sql_c);
			ffi::kdf_string_free(sql_c);

			ffi::FFI_ArrowArray array {};
			ffi::FFI_ArrowSchema schema {};
			if (!RunSqlToArrow(context, reduce_sql, &array, &schema)) {
				// Do NOT free `sm` here: the catch-all below is the sole owner and frees it on every
				// throwing path. Freeing here too would double-free `sm` when the catch runs.
				throw IOException("delta SM SDK: failed to execute reduce SQL in DuckDB");
			}
			if (ffi::kdf_sm_submit_reduce(sm, &array, &schema, &err) != 0) {
				ThrowFfi(err, "kdf_sm_submit_reduce");
			}
		}
		char *out = ffi::kdf_sm_result_sql(sm, &err);
		if (!out) {
			ThrowFfi(err, "kdf_sm_result_sql");
		}
		string sql(out);
		ffi::kdf_string_free(out);
		ffi::kdf_sm_free(sm);
		return sql;
	} catch (...) {
		ffi::kdf_sm_free(sm);
		throw;
	}
}

} // namespace delta_sdk
} // namespace duckdb
