//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan/delta_multi_file_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_functions.hpp"
#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"

namespace duckdb {

class StateWithBlockableTasks;

struct DeltaFileMetaData {
	DeltaFileMetaData() {};

	// No copying pls
	DeltaFileMetaData(const DeltaFileMetaData &) = delete;
	DeltaFileMetaData &operator=(const DeltaFileMetaData &) = delete;

	~DeltaFileMetaData() {
		if (selection_vector.ptr) {
			ffi::free_bool_slice(selection_vector);
		}
	}

	idx_t delta_snapshot_version = DConstants::INVALID_INDEX;
	idx_t file_number = DConstants::INVALID_INDEX;
	idx_t cardinality = DConstants::INVALID_INDEX;
	ffi::KernelBoolSlice selection_vector = {nullptr, 0};

	case_insensitive_map_t<Value> partition_map;

	unique_ptr<vector<unique_ptr<ParsedExpression>>> transform_expression;
};

// Constraint only for internal delta extension use
// Todo: refactor to use duckdb constraint classes, updating the DuckDB side NotNullConstraint
class NestedNotNullConstraint {
public:
	explicit NestedNotNullConstraint(LogicalIndex index_p, string path_p) : index(index_p), path(path_p) {
	}
	LogicalIndex index;
	string path;
};

//! The DeltaMultiFileList implements the MultiFileList API to allow injecting it into the regular DuckDB parquet scan
class DeltaMultiFileList : public SimpleMultiFileList {
	friend struct ScanDataCallBack;

public:
	DeltaMultiFileList(ClientContext &context, const string &path, idx_t version,
	                   optional_ptr<const DeltaMultiFileList> previous = nullptr);
	~DeltaMultiFileList();
	string GetPath() const;
	static string ToDuckDBPath(const string &raw_path);
	static string ToDeltaPath(const string &raw_path);

	//! MultiFileList API
public:
	void Bind(vector<LogicalType> &return_types, vector<string> &names);
	unique_ptr<MultiFileList> ComplexFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                MultiFilePushdownInfo &info,
	                                                vector<unique_ptr<Expression>> &filters) const override;

	unique_ptr<MultiFileList> DynamicFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                const vector<string> &names, const vector<LogicalType> &types,
	                                                const vector<column_t> &column_ids,
	                                                TableFilterSet &filters) const override;

	unique_ptr<DeltaMultiFileList> PushdownInternal(ClientContext &context, TableFilterSet &new_filters) const;

	vector<OpenFileInfo> GetAllFiles() const override;
	FileExpandResult GetExpandResult() const override;
	idx_t GetTotalFileCount() const override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context) const override;
	DeltaFileMetaData &GetMetaData(idx_t index) const;
	idx_t GetVersion();
	//! Set the time-travel target version before the snapshot is lazily resolved. Used by the reader's
	//! Bind(), since the `version` named parameter is parsed after the file list is constructed.
	void SetVersion(idx_t v) {
		version = v;
	}
	vector<string> GetPartitionColumns();

	//! One resolved file + its metadata, built independently of the shared list (lock-free).
	struct ResolvedFileEntry {
		OpenFileInfo file;
		unique_ptr<DeltaFileMetaData> meta;
	};

	//! Plan-based scan (design C′ build pipeline): build the resolved file + metadata for one surviving
	//! scan_file_row WITHOUT touching the shared list — no lock taken. `path_val` is the file path;
	//! `fcv_val` is the fileConstantValues struct (partition values / row tracking) or NULL; `dv_val` is
	//! the deletionVector struct or NULL. The (expensive) deletion-vector resolution happens here, so
	//! the build sink can run it lock-free across many threads. Returns an entry with an empty
	//! `file.path` when `path_val` is NULL (caller skips it).
	ResolvedFileEntry BuildFileEntry(const Value &path_val, const Value &fcv_val, const Value &dv_val) const;
	//! Append a batch of pre-built entries to the shared list under a single lock (one lock per build
	//! thread, not per row), assigning each its file_number.
	void AppendResolvedEntries(vector<ResolvedFileEntry> &&entries);
	//! Mark the build-pipeline-fed file list complete; the source side then serves resolved_files
	//! directly (no lazy nested-query path, no kernel iterator) and, since the list is now immutable,
	//! lock-free. Also sets the listing closed and wakes any source blocked at the list tail.
	void MarkExternallyPopulated();

	//! Streaming-sink mode (concurrent sink+source PhysicalDeltaLoad): the sink appends resolved
	//! entries via AppendResolvedEntries WHILE the source scans, instead of fully building before the
	//! source runs. Enables the GetFileInternal "serve resolved_files directly" path before the list is
	//! closed, and makes the list report itself as not-yet-finalized so the source BLOCKs (rather than
	//! finishing) when it catches up to the tail.
	void MarkStreamingPopulated();
	//! True once MarkStreamingPopulated ran: this list is fed by PhysicalDeltaLoad's sink (delta_load
	//! TVF). In this mode the reader must output the *physical* read schema (field-id mapped) — the
	//! kernel's terminal Transform does the physical->logical rename — so InitializeReader must NOT
	//! override the bound (physical) global columns with the snapshot's logical lazy_loaded_schema.
	bool IsStreamingPopulated() const {
		return streaming_populated;
	}

	//! Faithful Load (delta_load TVF): supply the read schema explicitly instead of resolving it from a
	//! kernel snapshot. The lowering (plan_to_sql) passes `file_schema`(+`field_ids`) and `base_url` as
	//! named params, so a Load over an arbitrary file set (commit/checkpoint/sidecar/data) never needs a
	//! Delta snapshot. `columns` are the physical read columns (names from file_schema, INTEGER field-id
	//! identifiers from field_ids) plus the appended metadata_derived broadcast columns. `partition_cols`
	//! are the declared broadcast/partition column names (usually empty — partitions are extracted from
	//! the broadcast fileConstantValues by the terminal Transform). When set, Bind /
	//! GetLazyLoadedGlobalColumns / GetPartitionColumns return these WITHOUT initializing any snapshot.
	void SetProvidedSchema(vector<DeltaMultiFileColumnDefinition> columns, vector<string> partition_cols);
	//! Register the source operator's blockable global state so the sink can wake it on append/close.
	//! Called once at source initialization. Set to null to clear.
	void SetBlockedSourceState(StateWithBlockableTasks *state);
	//! MultiFileList API: the streaming list is final only once the sink's Finalize has closed it.
	bool IsFinalizedListing() const override;

	vector<DeltaMultiFileColumnDefinition> &GetLazyLoadedGlobalColumns() const;
	vector<NestedNotNullConstraint> GetNestedNotNullConstraints() const;
	bool HasNullConstraintsInArrays() const;

protected:
	//! Get the i-th expanded file
	OpenFileInfo GetFile(idx_t i) const override;

protected:
	OpenFileInfo GetFileInternal(idx_t i) const;
	idx_t GetTotalFileCountInternal() const;
	void InitializeSnapshot() const;
	void InitializeScan() const;

public:
	//! Lower the kernel's metadata-only scan SM to the file-list reconciliation SQL — WITH the
	//! pushed-down `table_filters` threaded in as the kernel's
	//! data-skipping predicate, so the emitted SQL contains the stats-based file-skip FILTER (over
	//! `add.stats_parsed`). This does NOT execute the SQL: the optimizer parses+binds it into a VISIBLE
	//! child subplan of the delta_scan operator (READ_JSON/UNION/arg_max/FILTER... appear in EXPLAIN), and
	//! PhysicalDeltaLoad's streaming sink populates the file list from that subplan's rows. No lock required
	//! (reads only the immutable-after-bind GetPath()/version/global_columns/table_filters).
	string BuildReconciliationSQL(ClientContext &context) const;

	void EnsureSnapshotInitialized() const;
	void EnsureScanInitialized() const;

	void ReportFilterPushdown(ClientContext &context, DeltaMultiFileList &new_list, const vector<column_t> &column_ids,
	                          const char *log_type, optional_ptr<MultiFilePushdownInfo> mfr_info) const;

public: // TODO: clean up
	template <class T>
	T TryUnpackKernelResult(ffi::ExternResult<T> result) const {
		T return_value;
		auto res = KernelUtils::TryUnpackResult<T>(result, return_value);
		if (res.HasError()) {
			res.Throw();
		}
		return return_value;
	}

	mutable KernelExternEngine extern_engine;
	mutable shared_ptr<SharedKernelSnapshot> snapshot;

	mutable unique_ptr<DeltaLogPathArray> delta_log_path;

protected:
	// Note: Nearly this entire class is mutable because it represents a lazily expanded list of files that is logically
	//       const, but not physically.
	mutable mutex lock;
	mutable idx_t version;

	//! Delta Kernel Structures
	mutable shared_ptr<SharedKernelSnapshot> old_snapshot;

	mutable KernelScan scan;
	mutable KernelScanDataIterator scan_data_iterator;

	mutable vector<string> partitions;
	mutable vector<idx_t> partition_ids;

	//! Root path of the table, necessary for certain kernel calls
	mutable string root_path;

	//! Current file list resolution state
	mutable bool initialized_snapshot = false;
	mutable bool initialized_scan = false;
	mutable bool files_exhausted = false;
	//! Set once the file list for this list has been fully populated (by MarkExternallyPopulated).
	mutable bool plan_sql_done = false;

	//! Set when the file list was populated externally by the C′ build pipeline (PhysicalDeltaScan's
	//! sink). GetFileInternal then serves resolved_files directly.
	mutable bool externally_populated = false;

	//! Streaming sink+source mode: the sink appends to resolved_files concurrently with the source
	//! scanning. GetFileInternal serves resolved_files directly (like externally_populated) but the
	//! list is NOT immutable/closed until streaming_closed.
	mutable bool streaming_populated = false;
	//! Set by the sink's Finalize: no more files will be appended. Until then IsFinalizedListing()
	//! reports false so a source that catches up to the tail BLOCKs instead of finishing.
	mutable atomic<bool> streaming_closed {false};
	//! The source operator's blockable global state, registered once at source init, woken by the sink
	//! on each append and on close. Guarded by `lock` for set/clear; wake takes the state's own lock.
	mutable StateWithBlockableTasks *blocked_source_state = nullptr;
	//! Wake the registered source (if any) — takes the source state's own lock. Lock-order: callers
	//! must NOT hold `lock` while calling this (avoids inversion with the source-state lock).
	void WakeBlockedSource() const;

	//! Metadata map for files
	mutable vector<unique_ptr<DeltaFileMetaData>> metadata;

	mutable vector<OpenFileInfo> resolved_files;
	mutable TableFilterSet table_filters;

	mutable vector<NestedNotNullConstraint> not_null_constraints;
	mutable bool has_null_constraints_in_arrays = false;

	//! Global schema: NOTE: this might be missing some things
	vector<DeltaMultiFileColumnDefinition> global_columns;

	bool have_bound = false;

	//! Set by SetProvidedSchema: this list serves a delta_load (faithful Load node), so its bind schema,
	//! read schema (lazy_loaded_schema) and partition columns come from the explicitly-provided
	//! file_schema/field_ids/metadata_derived — NOT from a kernel snapshot. Plain delta_scan leaves this
	//! false and keeps the snapshot-driven path.
	bool schema_provided = false;
	//! Declared partition/broadcast column names for a delta_load (returned by GetPartitionColumns when
	//! schema_provided). Usually empty: partitions are extracted from the broadcast fileConstantValues.
	vector<string> provided_partitions;

	weak_ptr<ClientContext> client_ctx;

	// The schema containing the proper column identifiers, lazily loaded to avoid prematurely initializing the kernel
	// scan
	mutable vector<DeltaMultiFileColumnDefinition> lazy_loaded_schema;
};

// Callback for the ffi::kernel_scan_data_next callback
struct ScanDataCallBack {
	explicit ScanDataCallBack(const DeltaMultiFileList &snapshot_p) : snapshot(snapshot_p) {
	}
	static void VisitData(ffi::NullableCvoid engine_context, ffi::Handle<ffi::SharedScanMetadata> scan_metadata);
	static void VisitCallback(ffi::NullableCvoid engine_context, struct ffi::KernelStringSlice path, int64_t size,
	                          int64_t mod_time, const ffi::Stats *stats, const ffi::CDvInfo *dv_info,
	                          const ffi::Expression *transform, const struct ffi::CStringMap *partition_values);
	static void VisitCallbackInternal(ffi::NullableCvoid engine_context, struct ffi::KernelStringSlice path,
	                                  int64_t size, int64_t mod_time, const ffi::Stats *stats,
	                                  const ffi::CDvInfo *dv_info, const ffi::Expression *transform);

	const DeltaMultiFileList &snapshot;
	ErrorData error;
};

} // namespace duckdb
