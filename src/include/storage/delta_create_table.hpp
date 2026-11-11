//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_create_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "delta_utils.hpp"

#include "duckdb/common/file_system.hpp"
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

//! Refuses a partition column of a type whose values duckdb-delta cannot write into the log faithfully (BLOB)
void ThrowIfPartitionTypeUnwritable(const string &column_name, const LogicalType &type, const string &statement);

//! Resolves partition keys to the columns they name, case-insensitively as kernel does, in the columns' spelling.
//! Refuses a key that names no column, or a column twice, or a column of a type that is not primitive (STRUCT, LIST,
//! MAP, ARRAY, VARIANT, ...) or that ThrowIfPartitionTypeUnwritable refuses.
//! `clause` is PARTITIONED BY or PARTITION_BY.
vector<string> ResolvePartitionColumns(const ColumnList &columns, const vector<string> &keys, const string &statement,
                                       const string &clause);

//! Removes the `location` entry from `properties`, which CREATE's WITH and COPY's WITH_PROPERTIES both take as the
//! destination: it must name `target` (described in errors as `target_name`). Refuses `path`, and `location` twice.
void TakeLocationProperty(vector<pair<string, string>> &properties, const string &target, const string &target_name,
                          const string &statement);

//! Refuses every column that a Delta table DuckDB creates could not hold, or that DuckDB could not then write into it,
//! naming the column: a type the Delta schema cannot store (`type_hint` ends that message), a VARIANT below the top
//! level, a MAP with a LIST key, a collated VARCHAR. One column at a time, in column order; for each column the checks
//! run in the order above. A new refusal of this kind belongs here. CREATE and CREATE TABLE AS run it through
//! ThrowIfDeltaCannotCreateTable, COPY TO when it binds: on every path before the existing-table check (IF NOT EXISTS
//! included) and before any directory exists, so a statement fails the same way whether the table exists or not.
void ThrowIfDeltaCannotCreateThenWrite(const ColumnList &columns, const string &statement, const string &type_hint);

//! The refusals of a CREATE TABLE that need no client context: ThrowIfDeltaCannotCreateThenWrite on its columns, then
//! its PARTITIONED BY columns (ResolvePartitionColumns). DeltaCatalog::SupportsCreateTable runs them for CREATE, and
//! for CREATE TABLE AS on an existing table, which DuckDB may settle without calling the schema's CreateTable;
//! DeltaCatalog::PlanCreateTableAs runs them for CREATE TABLE AS on a fresh path. Bad options need a context to bind,
//! so BindDeltaCreateTable refuses them, after the conflict check for an existing table.
void ThrowIfDeltaCannotCreateTable(const CreateTableInfo &base, const string &statement, const string &type_hint);

//! Refuses a column with a VARIANT below its top level, naming it: the parquet writer writes VARIANT only as a
//! top-level column. INSERT calls it for a table another engine made; CREATE, CTAS and COPY refuse it through
//! ThrowIfDeltaCannotCreateThenWrite.
void ThrowIfVariantIsNested(const ColumnList &columns, const string &statement);

//! Whether a Delta table has been created at `path` yet, judged as kernel judges it: by the presence of
//! `_delta_log`. A cheap existence probe rather than a snapshot build.
bool DeltaTableExistsAt(ClientContext &context, const string &path);

//! Kernel rejects a table location that does not exist. Object stores conjure prefixes on write, but a local
//! filesystem needs the directory first. Returns whether this call created it.
bool EnsureTableDirectory(ClientContext &context, const string &path);

//! Removes `directory` if it is empty, and never what is in it: a file another writer adds keeps the directory. One
//! rmdir, so nothing can land between a check and the removal. Best effort: a directory that is missing, not empty,
//! not a directory, or cannot be removed is left as it is.
void TryRemoveEmptyDirectory(FileSystem &fs, const string &directory);

//! A kernel create-table transaction, held open from its build to its commit. Version 0 -- protocol, metadata and the
//! add actions of every file passed to AddFiles -- is written by Commit and by nothing else, so a transaction freed
//! without a commit leaves no log behind.
class DeltaCreateTableTransaction {
public:
	//! Makes a committer that commits version 0 through a catalog. Called once, right before the build consumes
	//! it, so that no error path before the build has a committer handle to free.
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
