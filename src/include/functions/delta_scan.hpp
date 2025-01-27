//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"
#include "duckdb/common/multi_file_reader.hpp"

namespace duckdb {
struct DeltaSnapshot;

struct DeltaFunctionInfo : public TableFunctionInfo {
	shared_ptr<DeltaSnapshot> snapshot;
	string expected_path;
	string table_name;
};

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
	case_insensitive_map_t<string> partition_map;
};

//! The DeltaSnapshot implements the MultiFileList API to allow injecting it into the regular DuckDB parquet scan
struct DeltaSnapshot : public MultiFileList {
	DeltaSnapshot(ClientContext &context, const string &path);
	string GetPath() const;
	static string ToDuckDBPath(const string &raw_path);
	static string ToDeltaPath(const string &raw_path);

	//! MultiFileList API
public:
	void Bind(vector<LogicalType> &return_types, vector<string> &names);
	unique_ptr<MultiFileList> ComplexFilterPushdown(ClientContext &context, const MultiFileReaderOptions &options,
	                                                MultiFilePushdownInfo &info,
	                                                vector<unique_ptr<Expression>> &filters) override;
	vector<string> GetAllFiles() override;
	FileExpandResult GetExpandResult() override;
	idx_t GetTotalFileCount() override;

	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context) override;
	idx_t GetVersion();
	DeltaFileMetaData &GetMetaData(idx_t index) const;

protected:
	//! Get the i-th expanded file
	string GetFile(idx_t i) override;

protected:
	string GetFileInternal(idx_t i);
	void InitializeSnapshot();
	void InitializeScan();

	template <class T>
	T TryUnpackKernelResult(ffi::ExternResult<T> result) {
		return KernelUtils::UnpackResult<T>(
		    result, StringUtil::Format("While trying to read from delta table: '%s'", paths[0]));
	}

	static void VisitData(void *engine_context, ffi::ExclusiveEngineData *engine_data,
	                      const struct ffi::KernelBoolSlice selection_vec);
	static void VisitCallback(ffi::NullableCvoid engine_context, struct ffi::KernelStringSlice path, int64_t size,
	                          const ffi::Stats *stats, const ffi::DvInfo *dv_info,
	                          const struct ffi::CStringMap *partition_values);

protected:
	mutable mutex lock;

	idx_t version;

	//! Delta Kernel Structures
	shared_ptr<SharedKernelSnapshot> snapshot;

	KernelExternEngine extern_engine;
	KernelScan scan;
	KernelGlobalScanState global_state;
	KernelScanDataIterator scan_data_iterator;

	//! Names
	vector<string> names;
	vector<LogicalType> types;
	bool have_bound = false;

	//! Metadata map for files
	vector<unique_ptr<DeltaFileMetaData>> metadata;

	//! Current file list resolution state
	bool initialized_snapshot = false;
	bool initialized_scan = false;

	bool files_exhausted = false;
	vector<string> resolved_files;
	TableFilterSet table_filters;

	ClientContext &context;
};

struct DeltaMultiFileReaderGlobalState : public MultiFileReaderGlobalState {
	DeltaMultiFileReaderGlobalState(vector<LogicalType> extra_columns_p, optional_ptr<const MultiFileList> file_list_p)
	    : MultiFileReaderGlobalState(extra_columns_p, file_list_p) {
	}
	//! The idx of the file number column in the result chunk
	idx_t delta_file_number_idx = DConstants::INVALID_INDEX;
	//! The idx of the file_row_number column in the result chunk
	idx_t file_row_number_idx = DConstants::INVALID_INDEX;

	void SetColumnIdx(const string &column, idx_t idx);
};

struct DeltaMultiFileReader : public MultiFileReader {
	static unique_ptr<MultiFileReader> CreateInstance(const TableFunction &table_function);
	//! Return a DeltaSnapshot
	shared_ptr<MultiFileList> CreateFileList(ClientContext &context, const vector<string> &paths,
	                                         FileGlobOptions options) override;

	//! Override the regular parquet bind using the MultiFileReader Bind. The bind from these are what DuckDB's file
	//! readers will try read
	bool Bind(MultiFileReaderOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
	          vector<string> &names, MultiFileReaderBindData &bind_data) override;

	//! Override the Options bind
	void BindOptions(MultiFileReaderOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
	                 vector<string> &names, MultiFileReaderBindData &bind_data) override;

	void CreateColumnMapping(const string &file_name, const vector<MultiFileReaderColumnDefinition> &local_columns,
	                         const vector<MultiFileReaderColumnDefinition> &global_columns,
	                         const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
	                         const MultiFileReaderBindData &bind_data, const string &initial_file,
	                         optional_ptr<MultiFileReaderGlobalState> global_state) override;

	unique_ptr<MultiFileReaderGlobalState>
	InitializeGlobalState(ClientContext &context, const MultiFileReaderOptions &file_options,
	                      const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
	                      const vector<MultiFileReaderColumnDefinition> &global_columns,
	                      const vector<ColumnIndex> &global_column_ids) override;

	void FinalizeBind(const MultiFileReaderOptions &file_options, const MultiFileReaderBindData &options,
	                  const string &filename, const vector<MultiFileReaderColumnDefinition> &local_columns,
	                  const vector<MultiFileReaderColumnDefinition> &global_columns,
	                  const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
	                  ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) override;

	//! Override the FinalizeChunk method
	void FinalizeChunk(ClientContext &context, const MultiFileReaderBindData &bind_data,
	                   const MultiFileReaderData &reader_data, DataChunk &chunk,
	                   optional_ptr<MultiFileReaderGlobalState> global_state) override;

	//! Override the ParseOption call to parse delta_scan specific options
	bool ParseOption(const string &key, const Value &val, MultiFileReaderOptions &options,
	                 ClientContext &context) override;

	// A snapshot can be injected into the multifilereader, this ensures the GetMultiFileList can return this snapshot
	// (note that the path should match the one passed to CreateFileList)
	shared_ptr<DeltaSnapshot> snapshot;
};

} // namespace duckdb
