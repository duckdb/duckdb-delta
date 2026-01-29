//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/delta_insert.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"

namespace duckdb {

class DeltaInsert : public PhysicalOperator {
public:
    //! INSERT INTO
    DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, TableCatalogEntry &table, physical_index_vector_t<idx_t> column_index_map);
    //! CREATE TABLE AS
    DeltaInsert(PhysicalPlan &plan, LogicalOperator &op, SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info);

	//! The table to insert into
	optional_ptr<TableCatalogEntry> table;
	//! Table schema, in case of CREATE TABLE AS
	optional_ptr<SchemaCatalogEntry> schema;
	//! Create table info, in case of CREATE TABLE AS
	unique_ptr<BoundCreateTableInfo> info;
	//! column_index_map
	physical_index_vector_t<idx_t> column_index_map;
    //! The physical copy used internally by this insert
    unique_ptr<PhysicalOperator> physical_copy_to_file;

public:
	// // Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

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
    idx_t partition_column_idx;
    string partition_value;
};

struct DeltaDataFile {
    DeltaDataFile() = default;
    // DeltaDataFile(const DeltaDataFile &other);
    // DeltaDataFile &operator=(const DeltaDataFile &);

    string file_name;
    idx_t row_count;
    idx_t file_size_bytes;
    idx_t footer_size;
    vector<DeltaPartition> partition_values;
};

} // namespace duckdb
