//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan/delta_multi_file_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"

namespace duckdb {

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

//! The DeltaMultiFileList implements the MultiFileList API to allow injecting it into the regular DuckDB parquet scan
class DeltaMultiFileList : public MultiFileList {
	friend struct ScanDataCallBack;

public:
	DeltaMultiFileList(ClientContext &context, const string &path);
	string GetPath() const;
	static string ToDuckDBPath(const string &raw_path);
	static string ToDeltaPath(const string &raw_path);

	//! MultiFileList API
public:
	void Bind(vector<LogicalType> &return_types, vector<string> &names);
	unique_ptr<MultiFileList> ComplexFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                MultiFilePushdownInfo &info,
	                                                vector<unique_ptr<Expression>> &filters) override;

	unique_ptr<MultiFileList> DynamicFilterPushdown(ClientContext &context, const MultiFileOptions &options,
	                                                const vector<string> &names, const vector<LogicalType> &types,
	                                                const vector<column_t> &column_ids,
	                                                TableFilterSet &filters) const override;

	unique_ptr<DeltaMultiFileList> PushdownInternal(ClientContext &context, TableFilterSet &new_filters) const;

	vector<OpenFileInfo> GetAllFiles() override;
	FileExpandResult GetExpandResult() override;
	idx_t GetTotalFileCount() override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context) override;
	DeltaFileMetaData &GetMetaData(idx_t index) const;
	idx_t GetVersion();
	vector<string> GetPartitionColumns();

	vector<MultiFileColumnDefinition> &GetLazyLoadedGlobalColumns() const;

protected:
	//! Get the i-th expanded file
	OpenFileInfo GetFile(idx_t i) override;

protected:
	OpenFileInfo GetFileInternal(idx_t i) const;
	idx_t GetTotalFileCountInternal() const;
	void InitializeSnapshot() const;
	void InitializeScan() const;

	void EnsureSnapshotInitialized() const;
	void EnsureScanInitialized() const;

	void ReportFilterPushdown(ClientContext &context, DeltaMultiFileList &new_list, const vector<column_t> &column_ids,
	                          const char *log_type, optional_ptr<MultiFilePushdownInfo> mfr_info) const;

	template <class T>
	T TryUnpackKernelResult(ffi::ExternResult<T> result) const {
		T return_value;
		auto res = KernelUtils::TryUnpackResult<T>(result, return_value);
		if (res.HasError()) {
			res.Throw();
		}
		return return_value;
	}

protected:
	// Note: Nearly this entire class is mutable because it represents a lazily expanded list of files that is logically
	//       const, but not physically.
	mutable mutex lock;
	mutable idx_t version;

	//! Delta Kernel Structures
	mutable shared_ptr<SharedKernelSnapshot> snapshot;
	mutable KernelExternEngine extern_engine;
	mutable KernelScan scan;
	mutable KernelGlobalScanState global_state;
	mutable KernelScanDataIterator scan_data_iterator;

	mutable vector<string> partitions;
	mutable vector<idx_t> partition_ids;

	//! Current file list resolution state
	mutable bool initialized_snapshot = false;
	mutable bool initialized_scan = false;
	mutable bool files_exhausted = false;

	//! Metadata map for files
	mutable vector<unique_ptr<DeltaFileMetaData>> metadata;

	mutable vector<OpenFileInfo> resolved_files;
	mutable TableFilterSet table_filters;

	//! Names
	vector<string> names;
	vector<LogicalType> types;

	bool have_bound = false;

	ClientContext &context;

	// The schema containing the proper column identifiers, lazily loaded to avoid prematurely initializing the kernel
	// scan
	mutable vector<MultiFileColumnDefinition> lazy_loaded_schema;
};

// Callback for the ffi::kernel_scan_data_next callback
struct ScanDataCallBack {
	explicit ScanDataCallBack(const DeltaMultiFileList &snapshot_p) : snapshot(snapshot_p) {
	}
	static void VisitData(ffi::NullableCvoid engine_context, ffi::Handle<ffi::SharedScanMetadata> scan_metadata);
	static void VisitCallback(ffi::NullableCvoid engine_context, struct ffi::KernelStringSlice path, int64_t size,
	                          const ffi::Stats *stats, const ffi::DvInfo *dv_info, const ffi::Expression *transform,
	                          const struct ffi::CStringMap *partition_values);
	static void VisitCallbackInternal(ffi::NullableCvoid engine_context, struct ffi::KernelStringSlice path,
	                                  int64_t size, const ffi::Stats *stats, const ffi::DvInfo *dv_info,
	                                  const ffi::Expression *transform);

	const DeltaMultiFileList &snapshot;
	ErrorData error;
};

} // namespace duckdb
