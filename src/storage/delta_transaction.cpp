#include "storage/delta_transaction.hpp"

#include "functions/delta_scan/delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include <duckdb/main/client_data.hpp>

#include "storage/delta_catalog.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/arrow/appender/append_data.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "functions/delta_scan/delta_scan.hpp"
#include "storage/delta_insert.hpp"
#include "storage/delta_table_entry.hpp"

namespace duckdb {

DeltaTransaction::DeltaTransaction(DeltaCatalog &delta_catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), access_mode(delta_catalog.access_mode) {
}

DeltaTransaction::~DeltaTransaction() {
}

void DeltaTransaction::Start() {
	transaction_state = DeltaTransactionState::TRANSACTION_NOT_YET_STARTED;
}

static void *allocate_string(const struct ffi::KernelStringSlice slice) {
    return new string(slice.ptr, slice.len);
}

struct CommitInfo {
    static vector<LogicalType> GetTypes() {
        return {LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)};
    };
    static vector<string> GetNames() {
        return {"engineCommitInfo"};
    };

    CommitInfo() {
        buffer.Initialize(Allocator::DefaultAllocator(), GetTypes());
    }

    void Append(Value commit_info_map) {
        idx_t current_size = buffer.size();
        idx_t current_capacity = buffer.GetCapacity();

        if (current_size == current_capacity) {
            buffer.SetCapacity(2*current_capacity);
        }

        buffer.SetValue(0, current_size, commit_info_map);
        buffer.SetCardinality(current_size+1);
    }

    void (*release)();
    static void InstrumentedRelease(ArrowArray *arg1) {
        auto holder = static_cast<ArrowAppendData *>(arg1->private_data);

        if (holder->options.client_context) {
            DUCKDB_LOG_TRACE(*holder->options.client_context, "Delta ToArrow debug: released CommitInfo");
        }

        return ArrowAppender::ReleaseArray(arg1);
    }

    ffi::ArrowFFIData ToArrow(optional_ptr<ClientContext> context) {
        if (context) {
            DUCKDB_LOG_TRACE(*context, "Delta ToArrow debug: created CommitInfo");
        }

        ffi::ArrowFFIData ffi_data;
        unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_types;
        ClientProperties props("UTC", ArrowOffsetSize::REGULAR, false, false, false, ArrowFormatVersion::V1_0, context);
        ArrowConverter::ToArrowArray(buffer, (ArrowArray*)(&ffi_data.array), props, extension_types);
        ArrowConverter::ToArrowSchema((ArrowSchema*)(&ffi_data.schema), GetTypes(), GetNames(), props);

        ffi_data.array.release = reinterpret_cast<void (*)(ffi::FFI_ArrowArray *)>(InstrumentedRelease);
        return ffi_data;
    }

    DataChunk buffer;
};

struct WriteMetaData {
    static LogicalType GetStatsType() {
        return LogicalType::STRUCT(child_list_t<LogicalType>({
            {"numRecords", LogicalType::BIGINT},
            {"tightBounds", LogicalType::BOOLEAN}
        }));
    }

    static Value CreateStatsValue(idx_t num_rows, bool tight_bounds) {
        return Value::STRUCT(GetStatsType(), {Value::BIGINT(num_rows), Value(tight_bounds)});
    }

    static vector<LogicalType> GetTypes() {
        // TODO: this needs to be in the schema of the file to write
        // stats: struct
        //     |    |-- numRecords: long
        //     |    |-- tightBounds: boolean
        //     |    |-- minValues: struct
        //     |    |    |-- a: struct
        //     |    |    |    |-- b: struct
        //     |    |    |    |    |-- c: long
        //     |    |-- maxValues: struct
        //     |    |    |-- a: struct
        //     |    |    |    |-- b: struct
        //     |    |    |    |    |-- c: long

        return {
            LogicalType::VARCHAR,
            LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
            LogicalType::BIGINT,
            LogicalType::BIGINT,
            GetStatsType()
        };
    };

    static vector<string> GetNames() {
        return {
            "path",
            "partitionValues",
            "size",
            "modificationTime",
            "stats"
        };
    };

    WriteMetaData() {
        buffer = make_uniq<DataChunk>();
        buffer->Initialize(Allocator::DefaultAllocator(), GetTypes());
    }

    WriteMetaData(DeltaMultiFileList &snapshot, vector<DeltaDataFile> &outstanding_appends) : WriteMetaData() {
        for (const auto &file : outstanding_appends) {
            auto table_path = snapshot.GetPath();
            auto file_without_double_slash = StringUtil::Replace(file.file_name, "\\", "/");
            // auto file_split = StringUtil::Split(file, "/");
            // auto file_name = file_split[file_split.size()-1];
            auto file_name = file.file_name.substr(table_path.size());
            InsertionOrderPreservingMap<string> partitions = {};

            // TODO: probably horribly wrong
            for (const auto &part : file.partition_values) {
                partitions.insert({snapshot.GetPartitionColumns()[part.partition_column_idx], part.partition_value});
            }

            Append(file_name, Value::MAP(partitions), file.row_count, Timestamp::GetCurrentTimestamp().value, true);
        }
    }

    void Append(const string &path, Value partition_values, idx_t size, idx_t modification_time, bool data_change) {
        idx_t current_size = buffer->size();
        idx_t current_capacity = buffer->GetCapacity();

        if (current_size == current_capacity) {
            buffer->SetCapacity(2*current_capacity);
        }

        buffer->SetValue(0, current_size, path);
        buffer->SetValue(1, current_size, partition_values);
        buffer->SetValue(2, current_size, Value::BIGINT(size));
        buffer->SetValue(3, current_size, Value::BIGINT(modification_time));
        buffer->SetValue(4, current_size, CreateStatsValue(size, true));
        buffer->SetCardinality(current_size+1);
    }

    void (*release)();
    static void InstrumentedRelease(ArrowArray *arg1) {
        auto holder = static_cast<ArrowAppendData *>(arg1->private_data);

        if (holder->options.client_context) {
            DUCKDB_LOG_TRACE(*holder->options.client_context, "Delta ToArrow debug: released WriteMetaData");
        }

        return ArrowAppender::ReleaseArray(arg1);
    }

    ffi::ArrowFFIData ToArrow(ClientContext &context) {
        DUCKDB_LOG_TRACE(context, "Delta ToArrow debug: created WriteMetaData");

        ffi::ArrowFFIData ffi_data;
        unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_types;
        ClientProperties props("UTC", ArrowOffsetSize::REGULAR, false, false, false, ArrowFormatVersion::V1_0, context);
        ArrowConverter::ToArrowArray(*buffer, (ArrowArray*)(&ffi_data.array), props, extension_types);
        ArrowConverter::ToArrowSchema((ArrowSchema*)(&ffi_data.schema), GetTypes(), GetNames(), props);

        ffi_data.array.release = reinterpret_cast<void (*)(ffi::FFI_ArrowArray *)>(InstrumentedRelease);

        return ffi_data;
    }

    unique_ptr<DataChunk> buffer;
};

vector<DeltaMultiFileColumnDefinition> DeltaTransaction::GetWriteSchema(ClientContext &context) {
    if (transaction_state == DeltaTransactionState::TRANSACTION_NOT_YET_STARTED) {
        InitializeTransaction(context);
    }

    auto write_context = ffi::get_write_context(kernel_transaction.get());
    auto result = SchemaVisitor::VisitWriteContextSchema(write_context, write_entry.get()->snapshot->VariantEnabled());
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


void DeltaTransaction::Commit(ClientContext &context) {
	if (transaction_state == DeltaTransactionState::TRANSACTION_STARTED) {
		transaction_state = DeltaTransactionState::TRANSACTION_FINISHED;

	    if (!outstanding_appends.empty()) {
	        auto write_context = ffi::get_write_context(kernel_transaction.get());
	        auto write_path = ffi::get_write_path(write_context, allocate_string);

	        string write_path_string;
	        if (write_path) {
	            write_path_string = *(string*)write_path;
	            delete (string*)write_path;
	        }

	        // Create metadata from the current outstanding appends
	        WriteMetaData write_metadata(*table_entry->snapshot, outstanding_appends);
	        // Convert write metadata to ArrowFFI
	        auto write_metadata_ffi = write_metadata.ToArrow(context);

            // Convert to Delta Kernel EngineData
	        KernelEngineData write_metadata_engine_data = table_entry->snapshot->TryUnpackKernelResult(ffi::get_engine_data(write_metadata_ffi.array, &write_metadata_ffi.schema, DuckDBEngineError::AllocateError));

	        // Add the write data to the commit
	        ffi::add_files(kernel_transaction.get(), write_metadata_engine_data.release());

	        table_entry->snapshot->TryUnpackKernelResult(ffi::commit(kernel_transaction.release(), table_entry->snapshot->extern_engine.get()));
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
    if (access_mode == AccessMode::READ_ONLY) {
        throw InvalidInputException("Can not append to a read only table");
    }
    transaction_state = DeltaTransactionState::TRANSACTION_STARTED;

    D_ASSERT(table_entry);

    // Start the kernel transaction
    string path =  table_entry->snapshot->GetPath();
    auto path_slice = KernelUtils::ToDeltaString(path);
    auto new_kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(ffi::transaction(path_slice, table_entry->snapshot->extern_engine.get()));

    // Create commit info
    CommitInfo commit_info;
    commit_info.Append(Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, {Value("engineInfo")}, {Value("DuckDB")}));
    auto commit_info_arrow = commit_info.ToArrow(context);

    // Convert arrow to Engine Data
    KernelEngineData commit_info_engine_data = table_entry->snapshot->TryUnpackKernelResult(ffi::get_engine_data(commit_info_arrow.array, &commit_info_arrow.schema, DuckDBEngineError::AllocateError));

    string engine_info = "DuckDB";
    kernel_transaction = table_entry->snapshot->TryUnpackKernelResult(ffi::with_engine_info(new_kernel_transaction, KernelUtils::ToDeltaString(engine_info), table_entry->snapshot->extern_engine.get()));
    write_entry = table_entry.get();
}

void DeltaTransaction::Append(ClientContext &context, const vector<DeltaDataFile> &append_files) {
    if (transaction_state == DeltaTransactionState::TRANSACTION_NOT_YET_STARTED) {
        InitializeTransaction(context);
    }

    // Append the newly inserted data
    outstanding_appends.insert(outstanding_appends.end(), append_files.begin(), append_files.end());
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

DeltaTableEntry &DeltaTransaction::InitializeTableEntry(ClientContext &context, DeltaSchemaEntry &schema_entry, idx_t version) {
	unique_lock<mutex> lck(lock);

    // Latest version
    if (version == DConstants::INVALID_INDEX) {
        if (!table_entry) {
            table_entry = schema_entry.CreateTableEntry(context, version);
        }
        return *table_entry;
    }

    // Specific version
    auto lookup = versioned_table_entries.find(version);
    if (lookup != versioned_table_entries.end()) {
        return *lookup->second;
    }
    auto new_entry = schema_entry.CreateTableEntry(context, version);
    return *(versioned_table_entries[version] = std::move(new_entry));
}

} // namespace duckdb
