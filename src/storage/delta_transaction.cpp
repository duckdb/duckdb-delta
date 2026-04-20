#include "storage/delta_transaction.hpp"

#include "duckdb/common/helper.hpp"
#include "path_utils.hpp"
#include "functions/delta_scan/delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include <duckdb/main/client_data.hpp>

#include "storage/delta_catalog.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/arrow/appender/append_data.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "functions/delta_scan/delta_scan.hpp"
#include "storage/delta_insert.hpp"
#include "duckdb/main/connection.hpp"
#include "storage/delta_table_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"

namespace duckdb {

DeltaTransaction::DeltaTransaction(DeltaCatalog &delta_catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), access_mode(delta_catalog.access_mode), parent_commit(delta_catalog.parent_commit),
      parent_catalog_name(delta_catalog.parent_catalog_name), unity_table_id(delta_catalog.unity_table_id) {
	commit_function = delta_catalog.commit_function;
}

DeltaTransaction::~DeltaTransaction() {
}

void DeltaTransaction::Start() {
	transaction_state = DeltaTransactionState::TRANSACTION_NOT_YET_STARTED;
}

static void *allocate_string(const struct ffi::KernelStringSlice slice) {
	return new string(slice.ptr, slice.len);
}

struct DeltaCommitInfo {
public:
	DeltaCommitInfo() {
		buffer.Initialize(Allocator::DefaultAllocator(), GetTypes());
		buffer.SetCardinality(0);
	}

public:
	static vector<LogicalType> GetTypes() {
		return {LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)};
	};
	static vector<string> GetNames() {
		return {"engineCommitInfo"};
	};

public:
	void Append(Value commit_info_map) {
		idx_t current_size = buffer.size();
		idx_t current_capacity = buffer.GetCapacity();

		if (current_size == current_capacity) {
			buffer.SetCapacity(2 * current_capacity);
		}

		buffer.SetValue(0, current_size, commit_info_map);
		buffer.SetCardinality(current_size + 1);
	}

	void (*release)();
	static void InstrumentedRelease(ArrowArray *arg1) {
		LoggerCallback::TryLog("delta", LogLevel::LOG_TRACE, "Delta ToArrow debug: released CommitInfo");
		return ArrowAppender::ReleaseArray(arg1);
	}

	ffi::ArrowFFIData ToArrow(optional_ptr<ClientContext> context) {
		LoggerCallback::TryLog("delta", LogLevel::LOG_TRACE, "Delta ToArrow debug: created CommitInfo");

		ffi::ArrowFFIData ffi_data;
		unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_types;
		ClientProperties props("UTC", ArrowOffsetSize::REGULAR, false, false, false, ArrowFormatVersion::V1_0, context);
		ArrowConverter::ToArrowArray(buffer, (ArrowArray *)(&ffi_data.array), props, extension_types);
		ArrowConverter::ToArrowSchema((ArrowSchema *)(&ffi_data.schema), GetTypes(), GetNames(), props);

		ffi_data.array.release = reinterpret_cast<void (*)(ffi::FFI_ArrowArray *)>(InstrumentedRelease);
		return ffi_data;
	}

private:
	DataChunk buffer;
};

struct StatNode {
	// If leaf node contains value
	DeltaColumnStats stats;
	LogicalType type;
	unordered_map<string, StatNode> children;
};

static LogicalType ParseInnerType(const LogicalType &root_type, const vector<string> &name, idx_t offset) {
	if (root_type.IsNested() && name.size() == offset) {
		throw InternalException("Invalid stats name: empty");
	}
	if (root_type.id() == LogicalTypeId::STRUCT) {
		auto &children = StructType::GetChildTypes(root_type);
		for (auto &child : children) {
			if (child.first == name[offset]) {
				return ParseInnerType(child.second, name, offset + 1);
			}
		}
		throw InternalException("Invalid stats name: did not find expected child: %s", name[offset]);
	} else {
		return root_type;
	}
}

// Converts the stats from a.b.c -> colstat to a nested StatNode tree
static void ParseStatsType(const vector<string> &name, idx_t offset, DeltaColumnStats &stats,
                           unordered_map<string, StatNode> &output) {
	if (name.size() <= offset) {
		throw InternalException("Invalid stats name: empty");
	}

	bool is_leaf = (name.size() == 1 + offset);

	if (output.find(name[offset]) != output.end()) {
		// Non-leaf collision means a sibling field already created this parent node — merge into it
		if (is_leaf) {
			throw InternalException("Invalid stats name: duplicate leaf '%s'", name[offset]);
		}
		return ParseStatsType(name, offset + 1, stats, output[name[offset]].children);
	}

	output[name[offset]] = StatNode();

	// We are at the leaf
	if (is_leaf) {
		output[name[offset]].stats = stats;
		output[name[offset]].type = ParseInnerType(stats.root_type, name, 1);
		return;
	}

	return ParseStatsType(name, offset + 1, stats, output[name[offset]].children);
}

static Value CreateValueLogicalTypeFromStatNode(const unordered_map<string, StatNode> &tree, const string &field) {
	child_list_t<Value> children;

	for (const auto &node : tree) {
		if (node.second.children.size() == 0) {
			if (field == "min") {
				children.push_back({node.first, node.second.stats.has_min
				                                    ? Value(node.second.stats.min).DefaultCastAs(node.second.type)
				                                    : Value(node.second.type)});
			} else if (field == "max") {
				children.push_back({node.first, node.second.stats.has_max
				                                    ? Value(node.second.stats.max).DefaultCastAs(node.second.type)
				                                    : Value(node.second.type)});
			} else if (field == "null_count") {
				children.push_back({node.first, node.second.stats.has_null_count
				                                    ? Value::BIGINT(node.second.stats.null_count)
				                                    : Value(LogicalType::BIGINT)});
			} else {
				throw InternalException("Invalid field: %s", field.c_str());
			}
		} else {
			children.push_back({node.first, CreateValueLogicalTypeFromStatNode(node.second.children, field)});
		}
	}

	// TODO: support lists and other madness
	return Value::STRUCT(children);
}

struct WriteMetaData {
	static LogicalType GetStatsType(optional_ptr<const DeltaDataFile> file) {
		if (file && !file->column_stats.empty()) {
			unordered_map<string, StatNode> result;
			for (auto stat : file->column_stats) {
				ParseStatsType(stat.first, 0, stat.second, result);
			}

			return LogicalType::STRUCT(child_list_t<LogicalType>({
			    {"numRecords", LogicalType::BIGINT},
			    {"nullCount", CreateValueLogicalTypeFromStatNode(result, "null_count").type()},
			    {"minValues", CreateValueLogicalTypeFromStatNode(result, "min").type()},
			    {"maxValues", CreateValueLogicalTypeFromStatNode(result, "max").type()},
			    {"tightBounds", LogicalType::BOOLEAN},
			}));
		}

		return LogicalType::STRUCT(
		    child_list_t<LogicalType>({{"numRecords", LogicalType::BIGINT}, {"tightBounds", LogicalType::BOOLEAN}}));
	}

	static Value CreateStatsValue(const DeltaDataFile &file, bool tight_bounds) {
		if (file.column_stats.empty()) {
			return Value::STRUCT(GetStatsType(nullptr), {Value::BIGINT(file.row_count), Value(tight_bounds)});
		}

		unordered_map<string, StatNode> result;
		for (auto stat : file.column_stats) {
			ParseStatsType(stat.first, 0, stat.second, result);
		}

		return Value::STRUCT(GetStatsType(&file), {
		                                              Value::BIGINT(file.row_count),
		                                              CreateValueLogicalTypeFromStatNode(result, "null_count"),
		                                              CreateValueLogicalTypeFromStatNode(result, "min"),
		                                              CreateValueLogicalTypeFromStatNode(result, "max"),
		                                              Value(tight_bounds),
		                                          });
	}

	static vector<LogicalType> GetTypes(optional_ptr<const DeltaDataFile> file) {
		return {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR), LogicalType::BIGINT,
		        LogicalType::BIGINT, GetStatsType(file)};
	};
	static vector<string> GetNames() {
		return {"path", "partitionValues", "size", "modificationTime", "stats"};
	};

	WriteMetaData(DeltaMultiFileList &snapshot, vector<DeltaDataFile> &outstanding_appends) {
		const DeltaDataFile *first_file = outstanding_appends.empty() ? nullptr : &outstanding_appends[0];
		buffer_types = GetTypes(first_file);
		buffer = make_uniq<DataChunk>();
		buffer->Initialize(Allocator::DefaultAllocator(), buffer_types);

		for (const auto &file : outstanding_appends) {
			auto table_path = snapshot.GetPath();

			// consume any leading '/' chars to be certain path is relative -- as seen in #268 they corrupt (for spark)
			// https://github.com/duckdb/duckdb-delta/issues/268
			auto file_name_offset = table_path.size();
			for (; file.file_name[file_name_offset] == '/'; ++file_name_offset) {
			}
			auto file_name = file.file_name.substr(file_name_offset);
			D_ASSERT(!StringUtil::StartsWith(file_name, "/"));

			InsertionOrderPreservingMap<string> partitions = {};
			for (const auto &part : file.partition_values) {
				partitions.insert({snapshot.GetPartitionColumns()[part.partition_column_idx], part.partition_value});
			}

			Append(file_name, Value::MAP(partitions), file);
		}
	}

	void Append(const string &path, Value partition_values, const DeltaDataFile &file) {
		idx_t current_size = buffer->size();
		idx_t current_capacity = buffer->GetCapacity();

		if (current_size == current_capacity) {
			buffer->SetCapacity(2 * current_capacity);
		}

		auto stats = CreateStatsValue(file, true);

		buffer->SetValue(0, current_size, path);
		buffer->SetValue(1, current_size, partition_values);
		buffer->SetValue(2, current_size, Value::BIGINT(file.file_size_bytes));
		buffer->SetValue(3, current_size, Value::BIGINT(Timestamp::GetEpochMs(file.last_modified_time)));
		buffer->SetValue(4, current_size, stats);
		buffer->SetCardinality(current_size + 1);
	}

	void (*release)();
	static void InstrumentedRelease(ArrowArray *arg1) {
		LoggerCallback::TryLog("delta", LogLevel::LOG_TRACE, "Delta ToArrow debug: released WriteMetaData");
		return ArrowAppender::ReleaseArray /**/ (arg1);
	}

	ffi::ArrowFFIData ToArrow(ClientContext &context) {
		LoggerCallback::TryLog("delta", LogLevel::LOG_TRACE, "Delta ToArrow debug: created WriteMetaData");

		ffi::ArrowFFIData ffi_data;
		unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_types;
		ClientProperties props("UTC", ArrowOffsetSize::REGULAR, false, false, false, ArrowFormatVersion::V1_0,
		                       optional_ptr<ClientContext>(&context));
		ArrowConverter::ToArrowArray(*buffer, (ArrowArray *)(&ffi_data.array), props, extension_types);
		ArrowConverter::ToArrowSchema((ArrowSchema *)(&ffi_data.schema), buffer_types, GetNames(), props);

		ffi_data.array.release = reinterpret_cast<void (*)(ffi::FFI_ArrowArray *)>(InstrumentedRelease);

		return ffi_data;
	}

	vector<LogicalType> buffer_types;
	unique_ptr<DataChunk> buffer;
};

vector<DeltaMultiFileColumnDefinition> DeltaTransaction::GetWriteSchema(ClientContext &context) {
	if (transaction_state == DeltaTransactionState::TRANSACTION_NOT_YET_STARTED) {
		InitializeTransaction(context);
	}

	auto write_context = ffi::get_write_context(kernel_transaction.get());
	auto result =
	    SchemaVisitor::VisitWriteContextSchema(write_entry.get()->snapshot->extern_engine.get(), write_context);
	return result;
}

void DeltaTransaction::CleanUpFiles() {
	// Clean up the files created by this transaction
	auto context_ptr = context.lock();
	if (context_ptr) {
		for (const auto &append : outstanding_appends) {
			auto &fs = FileSystem::GetFileSystem(*context_ptr);
			fs.TryRemoveFile(append.file_name);
		}
	}
	outstanding_appends.clear();
}

ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>> DeltaTransaction::CommitCallback(ffi::NullableCvoid context,
                                                                                           ffi::CommitRequest request) {
	auto transaction = reinterpret_cast<DeltaTransaction *>(context);

	try {
		auto current_context = transaction->current_context.lock();
		if (!current_context) {
			throw InternalException("No current client context in Catalog Commit Callback");
		}
		if (!transaction->write_entry) {
			throw InternalException("No write entry in Catalog Commit Callback");
		}
		if (!transaction->parent_table_entry) {
			throw InternalException("No parent table entry in Catalog Commit Callback");
		}

		// Extract commit info from the request
		if (request.commit_info.tag != ffi::OptionalValue<ffi::Commit>::Tag::Some) {
			throw InternalException("CommitCallback received request without commit_info");
		}

		// TODO (sam): This function is a little hacky right now, could be cleaned up

		auto &commit_info = request.commit_info.some._0;
		auto staged_commit_path_string = KernelUtils::FromDeltaString(commit_info.file_name);
		auto version = commit_info.version;
		auto timestamp_val = commit_info.timestamp;
		auto size = commit_info.file_size;
		auto file_modification_time = commit_info.file_modification_timestamp;

		child_list_t<Value> children = {
		    {"staged_commit_path", Value(staged_commit_path_string)},
		    {"staged_commit_size", Value::BIGINT(size)},
		    {"staged_commit_timestamp", Value::BIGINT(timestamp_val)},
		    {"version", Value::BIGINT(version)},
		    {"table_entry_pointer", Value::POINTER(CastPointerToValue(transaction->parent_table_entry.get()))},
		    {"file_modification_time", Value::BIGINT(file_modification_time)},
		};

		auto staged_commit_data = Value::STRUCT(children);

		DUCKDB_LOG_INTERNAL(*current_context, "delta.CatalogManagedCommit", LogLevel::LOG_DEBUG,
		                    staged_commit_data.ToString());

		// Invoke the commit function on the catalog
		DataChunk output;
		TableFunctionInput data = {nullptr, nullptr, nullptr};
		output.Initialize(*current_context, {staged_commit_data.type(), LogicalType::BOOLEAN}, 1);
		output.SetValue(0, 0, staged_commit_data);
		output.SetCardinality(1);

		if (!transaction->commit_function) {
			throw InternalException("No commit function found in Catalog Commit Callback");
		}
		// Special function that expects a 2-sized ANY datachunk containing the input on row 1 that will place the
		// output on row 2
		transaction->commit_function->functions.functions[0].function(*current_context, data, output);

		auto result = output.GetValue(1, 0);
		if (result.IsNull()) {
			// Commit conflict - return error string
			auto error_str = ffi::allocate_kernel_string(KernelUtils::ToDeltaString("Commit conflict"),
			                                             DuckDBEngineError::AllocateError);
			ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>> error_result;
			error_result.tag = ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>>::Tag::Some;
			error_result.some._0 = error_str.ok._0;
			return error_result;
		}

		// Success - return None
		ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>> success_result;
		success_result.tag = ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>>::Tag::None;
		return success_result;

	} catch (std::exception &e) {
		transaction->active_error = ErrorData(e);
		auto error_str = ffi::allocate_kernel_string(KernelUtils::ToDeltaString(transaction->active_error.Message()),
		                                             DuckDBEngineError::AllocateError);
		ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>> error_result;
		error_result.tag = ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>>::Tag::Some;
		error_result.some._0 = error_str.ok._0;
		return error_result;
	} catch (...) {
		string message = "Unknown error occurred when committing to a Unity Catalog managed commit";
		auto error_str =
		    ffi::allocate_kernel_string(KernelUtils::ToDeltaString(message), DuckDBEngineError::AllocateError);
		ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>> error_result;
		error_result.tag = ffi::OptionalValue<ffi::Handle<ffi::ExclusiveRustString>>::Tag::Some;
		error_result.some._0 = error_str.ok._0;
		return error_result;
	}
}

void DeltaTransaction::Commit(ClientContext &context) {
	if (transaction_state == DeltaTransactionState::TRANSACTION_STARTED) {
		transaction_state = DeltaTransactionState::TRANSACTION_FINISHED;

		if (!outstanding_appends.empty()) {
			// Finally we add the registered transaction versions
			for (const auto &app_version : app_versions) {
				auto app_id = app_version.first;
				auto app_version_info = app_version.second;
				auto new_version = app_version_info.new_version;
				auto expected_version = app_version_info.expected_version;

				// Verify that the previous version is correct still
				auto &snapshot = *table_entry->snapshot;
				auto kernel_snapshot = snapshot.snapshot->GetLockingRef();
				auto app_id_kernel_string = KernelUtils::ToDeltaString(app_id);
				auto get_app_id_version_result = ffi::get_app_id_version(kernel_snapshot.GetPtr(), app_id_kernel_string,
				                                                         snapshot.extern_engine.get());

				ffi::OptionalValue<int64_t> version_actual_opt;
				auto unpacked_version_result =
				    KernelUtils::TryUnpackResult(get_app_id_version_result, version_actual_opt);
				bool has_error = false;
				string error_version;
				if (unpacked_version_result.HasError()) {
					has_error = !expected_version.IsNull();
					if (has_error) {
						error_version = "ERROR";
					}
				}

				if (!has_error) {
					const auto actual_version = version_actual_opt.tag == ffi::OptionalValue<int64_t>::Tag::None
					                                ? Value()
					                                : Value(version_actual_opt.some._0);
					has_error = ((actual_version.IsNull() != expected_version.IsNull()) ||
					             (!actual_version.IsNull() && actual_version != expected_version));
					if (has_error) {
						error_version = actual_version.ToString();
					}
				}

				if (has_error) {
					throw TransactionException("DeltaTransaction version for app_id '%s' did not match the expected "
					                           "previous version of '%s' (found: '%s')",
					                           app_id, expected_version.ToString(), error_version);
				}

				kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(
				    ffi::with_transaction_id(kernel_transaction.release(), KernelUtils::ToDeltaString(app_id),
				                             new_version, table_entry->snapshot->extern_engine.get()));
			}

			// We have some special error handling here to ensure the error created by DuckDB is properly thrown here,
			// because we can't throw it across the FFI boundary, we need to store it in the transaction
			uint64_t commit_result;

			DUCKDB_LOG_INTERNAL(context, "delta.Commit", LogLevel::LOG_DEBUG, "Committing %s",
			                    table_entry->snapshot->GetPath());
			auto res = KernelUtils::TryUnpackResult(
			    ffi::commit(kernel_transaction.release(), table_entry->snapshot->extern_engine.get()), commit_result);
			if (res.HasError()) {
				if (active_error.HasError()) {
					active_error.Throw();
				} else {
					res.Throw();
				}
			}
		}
	}
}

void DeltaTransaction::Rollback() {
	if (transaction_state == DeltaTransactionState::TRANSACTION_STARTED) {
		transaction_state = DeltaTransactionState::TRANSACTION_FINISHED;
		CleanUpFiles();
	}
}

void DeltaTransaction::InitializeTransaction(ClientContext &context) {
	current_context = context.shared_from_this();

	if (access_mode == AccessMode::READ_ONLY) {
		throw InvalidInputException("Can not append to a read only table");
	}
	transaction_state = DeltaTransactionState::TRANSACTION_STARTED;

	D_ASSERT(table_entry);

	// Start the kernel transaction
	string path = table_entry->snapshot->GetPath();
	auto path_slice = KernelUtils::ToDeltaString(path);

	ffi::Handle<ffi::ExclusiveTransaction> new_kernel_transaction;

	{
		auto snapshot_ref = table_entry->snapshot->snapshot->GetLockingRef();

		if (parent_commit) {
			// Create UC commit client with callbacks, passing `this` as the context
			auto commit_client = ffi::get_uc_commit_client(this, CommitCallback);
			auto table_id = KernelUtils::ToDeltaString(unity_table_id.empty() ? path : unity_table_id);
			auto uc_committer = table_entry->snapshot->TryUnpackKernelResult(
			    ffi::get_uc_committer(commit_client, table_id, DuckDBEngineError::AllocateError));
			new_kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(ffi::transaction_with_committer(
			    snapshot_ref.GetPtr(), table_entry->snapshot->extern_engine.get(), uc_committer));
		} else {
			new_kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(
			    ffi::transaction(path_slice, table_entry->snapshot->extern_engine.get()));
		}
	}

	// Create commit info
	DeltaCommitInfo commit_info;
	commit_info.Append(
	    Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, {Value("engineInfo")}, {Value("DuckDB")}));
	auto commit_info_arrow = commit_info.ToArrow(context);

	// Convert arrow to Engine Data
	KernelEngineData commit_info_engine_data = table_entry->snapshot->TryUnpackKernelResult(
	    ffi::get_engine_data(commit_info_arrow.array, &commit_info_arrow.schema, DuckDBEngineError::AllocateError));

	string engine_info = "DuckDB";
	kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(ffi::with_engine_info(
	    new_kernel_transaction, KernelUtils::ToDeltaString(engine_info), table_entry->snapshot->extern_engine.get()));
	write_entry = table_entry.get();
}

void DeltaTransaction::Append(ClientContext &context, const vector<DeltaDataFile> &append_files) {
	if (transaction_state == DeltaTransactionState::TRANSACTION_NOT_YET_STARTED) {
		InitializeTransaction(context);
	}

	idx_t start = outstanding_appends.size();

	// Append the newly inserted data
	outstanding_appends.insert(outstanding_appends.end(), append_files.begin(), append_files.end());

	// TODO: this requires a round trip! we might already be able to optimize this
	// Note: file_size_bytes is already set from copy stats; we only need last_modified_time from the file system
	for (idx_t i = start; i < outstanding_appends.size(); i++) {
		auto &file = outstanding_appends[i];
		auto &fs = FileSystem::GetFileSystem(context);
		auto f = fs.OpenFile(file.file_name, FileOpenFlags::FILE_FLAGS_READ);
		file.last_modified_time = f->file_system.GetLastModifiedTime(*f);
	}

	if (!append_files.empty()) {
		// Build and add write metadata for new files per append; we do so here instead of in ::Commit
		// within Commit we no longer have an active transaction, which is required to build the arrow schema. We could
		// alternatively extend the Arrow API to support pre-build/cache the schema, but writing per append here is
		// simple.
		vector<DeltaDataFile> new_files(outstanding_appends.begin() + NumericCast<ptrdiff_t>(start),
		                                outstanding_appends.end());
		WriteMetaData write_metadata(*table_entry->snapshot, new_files);
		auto write_metadata_ffi = write_metadata.ToArrow(context);

		KernelEngineData write_metadata_engine_data = table_entry->snapshot->TryUnpackKernelResult(ffi::get_engine_data(
		    write_metadata_ffi.array, &write_metadata_ffi.schema, DuckDBEngineError::AllocateError));

		ffi::add_files(kernel_transaction.get(), write_metadata_engine_data.release());
	}
}

void DeltaTransaction::SetTransactionVersion(const string &app_id_p, idx_t new_version_p, Value expected_version_p) {
	app_versions.insert({app_id_p, {new_version_p, std::move(expected_version_p)}});
}

DeltaTransaction &DeltaTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<DeltaTransaction>();
}

AccessMode DeltaTransaction::GetAccessMode() const {
	return access_mode;
}

bool DeltaTransaction::HasOutstandingAppends() const {
	unique_lock<mutex> lck(lock);
	return !outstanding_appends.empty();
}

optional_ptr<DeltaTableEntry> DeltaTransaction::GetTableEntry(idx_t version) {
	unique_lock<mutex> lck(lock);

	if (version == DConstants::INVALID_INDEX) {
		return table_entry;
	}

	auto lookup = versioned_table_entries.find(version);

	if (lookup != versioned_table_entries.end()) {
		return lookup->second;
	}

	return nullptr;
}

DeltaTableEntry &DeltaTransaction::InitializeTableEntry(ClientContext &context, DeltaSchemaEntry &schema_entry,
                                                        idx_t version,
                                                        optional_ptr<const DeltaMultiFileList> old_snapshot) {
	unique_lock<mutex> lck(lock);

	// Latest version
	if (version == DConstants::INVALID_INDEX) {
		if (!table_entry) {
			table_entry = schema_entry.CreateTableEntry(context, version, old_snapshot);
		}
		return *table_entry;
	}

	// Specific version
	auto lookup = versioned_table_entries.find(version);
	if (lookup != versioned_table_entries.end()) {
		return *lookup->second;
	}

	auto new_entry = schema_entry.CreateTableEntry(context, version, old_snapshot);
	return *(versioned_table_entries[version] = std::move(new_entry));
}

} // namespace duckdb
