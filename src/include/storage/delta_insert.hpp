//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_insert.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/common/optional.hpp"
#include "delta_utils.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "storage/delta_create_table.hpp"

namespace duckdb {

struct DeltaCreateTableAsState;

class DeltaInsert : public PhysicalOperator {
public:
	//! INSERT INTO
	DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table,
	            physical_index_vector_t<idx_t> column_index_map);
	//! CREATE TABLE AS and COPY TO: writes a new table, whose version 0 commits with the files
	DeltaInsert(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
	            shared_ptr<DeltaCreateTableAsState> create_state);
	~DeltaInsert() override;

	//! The table to insert into
	optional_ptr<TableCatalogEntry> table;
	//! The create transaction and everything the write must undo should it fail, for CREATE TABLE AS and COPY TO
	shared_ptr<DeltaCreateTableAsState> create_state;
	//! column_index_map
	physical_index_vector_t<idx_t> column_index_map;

	//! Plans a write of `child`'s rows as version 0 of a new table: CREATE TABLE AS and COPY TO. `statement` names the
	//! statement in errors. Version 0 commits through `committer` when given (a catalog-managed table), else through
	//! the filesystem committer.
	static PhysicalOperator &
	PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, PhysicalOperator &child,
	                  DeltaCreateTableRequest request, const string &statement, idx_t estimated_cardinality,
	                  const DeltaCreateTableTransaction::CommitterFactory &committer = nullptr);

public:
	// // Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	// SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	// unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

struct DeltaPartition {
	//! The column's name as the copy wrote it, which is the key the log wants: the physical name on a mapped table
	string partition_column;
	optional<string> partition_value; // absent -> NULL
};

struct DeltaColumnStats {
	explicit DeltaColumnStats() = default;

	string min;
	string max;
	idx_t null_count = 0;
	idx_t num_values = 0;
	idx_t column_size_bytes = 0;
	bool contains_nan = false;
	bool has_null_count = false;
	bool has_num_values = false;
	bool has_min = false;
	bool has_max = false;
	bool any_valid = true;
	bool has_contains_nan = false;

	// Injected type to materialize stats with
	LogicalType root_type;
};

struct DeltaMultiFileColumnDefinition;

struct DeltaDataFile {
	DeltaDataFile() = default;
	// DeltaDataFile(const DeltaDataFile &other);
	// DeltaDataFile &operator=(const DeltaDataFile &);

	string file_name;
	idx_t row_count;
	idx_t file_size_bytes;
	idx_t footer_size;
	vector<DeltaPartition> partition_values;
	timestamp_t last_modified_time;

	// Stats per column, where column name is vector of path
	vector<pair<vector<string>, DeltaColumnStats>> column_stats;
};

//! The add actions of `files`, as kernel's add_files takes them. `table_path` is the prefix the copy wrote the files
//! under, which the add paths are relative to.
KernelEngineData CreateAddFilesEngineData(ClientContext &context, const string &table_path,
                                          const vector<DeltaDataFile> &files);

} // namespace duckdb
