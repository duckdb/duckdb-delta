//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_create_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"

#include "duckdb/parser/column_list.hpp"

namespace duckdb {
class DeltaCatalog;
struct CreateTableInfo;
struct DeltaDataFile;

//! Everything kernel's create-table builder takes, checked and evaluated
struct DeltaCreateTableRequest {
	string path;
	ColumnList columns;
	vector<string> partition_columns;
	vector<pair<string, string>> properties;
	//! Whether the create commit adds rows: true for CREATE TABLE AS and COPY, false for a schema-only CREATE
	bool data_change = false;
};

//! Checks a CREATE TABLE against what a Delta catalog can create and evaluates its options. Throws on anything
//! refused, before any kernel call or write.
DeltaCreateTableRequest BindDeltaCreateTable(ClientContext &context, DeltaCatalog &catalog,
                                             const CreateTableInfo &base);

//! Resolves partition keys to the columns they name, case-insensitively as kernel does, in the columns' spelling.
//! Refuses a key that names no column, or a column twice. `clause` is PARTITIONED BY or PARTITION_BY.
vector<string> ResolvePartitionColumns(const ColumnList &columns, const vector<string> &keys, const string &statement,
                                       const string &clause);

//! Removes the `location` entry from `properties`, which CREATE's WITH and COPY's WITH_PROPERTIES both take as the
//! destination: it must name `target` (described in errors as `target_name`). Refuses `path`, and `location` twice.
void TakeLocationProperty(vector<pair<string, string>> &properties, const string &target, const string &target_name,
                          const string &statement);

//! Refuses every column type that the Delta schema cannot store, naming the column. `hint` ends the message.
void ThrowIfDeltaCannotStore(const ColumnList &columns, const string &statement, const string &hint);

//! Refuses a column with a VARIANT below its top level, naming it: the parquet writer writes VARIANT only as a
//! top-level column. CREATE refuses it too, so that a table DuckDB creates is one DuckDB can write.
void ThrowIfVariantIsNested(const ColumnList &columns, const string &statement);

//! Whether a Delta table has been created at `path` yet, judged as kernel judges it: by the presence of
//! `_delta_log`. A cheap existence probe rather than a snapshot build.
bool DeltaTableExistsAt(ClientContext &context, const string &path);

//! Kernel rejects a table location that does not exist. Object stores conjure prefixes on write, but a local
//! filesystem needs the directory first. Returns whether this call created it.
bool EnsureTableDirectory(ClientContext &context, const string &path);

//! A kernel create-table transaction, held open from its build to its commit. Version 0 -- protocol, metadata and the
//! add actions of every file passed to AddFiles -- is written by Commit and by nothing else, so a transaction freed
//! without a commit leaves no log behind.
class DeltaCreateTableTransaction {
public:
	//! Makes a committer that commits version 0 through a catalog. Called once, right before the build consumes
	//! it: kernel has no free for a committer handle.
	using CommitterFactory = std::function<ffi::MutableCommitter *()>;

	//! Builds the transaction, through the filesystem committer unless `committer` is given
	DeltaCreateTableTransaction(ClientContext &context, const DeltaCreateTableRequest &request,
	                            const CommitterFactory &committer = nullptr);
	~DeltaCreateTableTransaction();

	DeltaCreateTableTransaction(const DeltaCreateTableTransaction &) = delete;
	DeltaCreateTableTransaction &operator=(const DeltaCreateTableTransaction &) = delete;

	const string &GetPath() const {
		return path;
	}
	bool IsOpen() const {
		return transaction != nullptr;
	}

	//! The table's columns in table order, as the data files must name them: with the physical names and field ids
	//! kernel assigned, on a column-mapped table. A partition column carries its physical name; it carries its field
	//! id only when the table's features require the partition columns in the files (PartitionColumnsMaterialized):
	//! otherwise kernel's write schemas leave it out.
	vector<DeltaMultiFileColumnDefinition> GetWriteColumns(ClientContext &context);

	//! Whether the data files must hold the partition columns (materializePartitionColumns, icebergCompatV2/V3), as
	//! GetWriteColumns found it
	bool PartitionColumnsMaterialized() const {
		return partition_columns_materialized;
	}

	//! Stages the add actions of written files in the transaction; nothing reaches the log until Commit
	void AddFiles(ClientContext &context, const vector<DeltaDataFile> &files);

	//! Writes version 0. Consumes the transaction, on error too.
	idx_t Commit(ClientContext &context);

private:
	string path;
	bool data_change;
	idx_t added_files = 0;
	vector<string> partition_columns;
	vector<LogicalType> partition_types;
	bool partition_columns_materialized = false;
	ColumnList columns;
	KernelExternEngine engine;
	ffi::ExclusiveCreateTableTransaction *transaction = nullptr;
};

} // namespace duckdb
