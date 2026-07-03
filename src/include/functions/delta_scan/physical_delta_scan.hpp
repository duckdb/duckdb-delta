//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/delta_scan/physical_delta_scan.hpp
//
// Plan-based scan (design C′): the data-stage scan as a first-class custom
// operator instead of a nested-Connection staple.
//
//   - `LogicalDeltaGet`  : a LogicalExtensionOperator injected via the delta_scan
//                          table function's `bind_operator` hook. It carries the
//                          bound table-function state and plans into a
//                          `PhysicalDeltaLoad`.
//   - `PhysicalDeltaLoad`: a source operator subclassing `PhysicalTableScan`, so
//                          it inherits the full parallel multi-file scan
//                          (row-group claiming, DeltaDeleteFilter, partition
//                          constants, column mapping) unchanged. Later milestones
//                          override `BuildPipelines` here to express the
//                          metadata-reconciliation build pipeline as a JOIN_BUILD
//                          dependency.
//
// M0 scope: prove the bind_operator -> LogicalExtensionOperator -> CreatePlan ->
// custom source path executes correctly. The file list is still resolved lazily
// inside `DeltaMultiFileList` (no build pipeline yet).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/column_index.hpp"
#include "duckdb/common/extra_operator_info.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

class DBConfig;

//! Register the delta_scan optimizer extension. It restores filter pushdown for the custom-operator
//! path (LogicalDeltaGet is invisible to DuckDB's FilterPushdown), so DELTA_KERNEL_PLAN_OP /
//! DELTA_KERNEL_PLAN_SM_ASYNC scans prune files (data skipping) like the standard path. No-op for every
//! other query.
void RegisterDeltaScanOptimizer(DBConfig &config);

//! Data-stage scan source operator. Subclasses PhysicalTableScan so the entire
//! parallel multi-file scan machinery (GetGlobalSourceState / GetLocalSourceState
//! / GetDataInternal, DeltaDeleteFilter, partition + field-id mapping) is reused
//! verbatim. M0 changes nothing functional vs PhysicalTableScan; it exists so we
//! own the operator and can override BuildPipelines in M1.
class PhysicalDeltaLoad : public PhysicalTableScan {
public:
	PhysicalDeltaLoad(PhysicalPlan &physical_plan, vector<LogicalType> types, TableFunction function,
	                  unique_ptr<FunctionData> bind_data, vector<LogicalType> returned_types,
	                  vector<ColumnIndex> column_ids, vector<idx_t> projection_ids, vector<string> names,
	                  unique_ptr<TableFilterSet> table_filters, idx_t estimated_cardinality, ExtraOperatorInfo extra_info,
	                  vector<Value> parameters, virtual_column_map_t virtual_columns)
	    : PhysicalTableScan(physical_plan, std::move(types), std::move(function), std::move(bind_data),
	                        std::move(returned_types), std::move(column_ids), std::move(projection_ids),
	                        std::move(names), std::move(table_filters), estimated_cardinality, std::move(extra_info),
	                        std::move(parameters), std::move(virtual_columns)) {
	}

	string GetName() const override {
		return "DYNAMIC_SCAN";
	}

	//===--------------------------------------------------------------------===//
	// Build/probe (design C′, M1): the metadata-reconciliation subplan is a build pipeline that
	// sinks into this operator; the data scan is the source/probe pipeline that depends on it.
	// Active only when this operator has a child (the metadata subplan); otherwise it is a plain
	// source (M0 behavior) and these are never called.
	//===--------------------------------------------------------------------===//
	bool IsSink() const override {
		return !children.empty();
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;

	//===--------------------------------------------------------------------===//
	// Streaming source: the data scan runs concurrently with the sink (no build-before-probe barrier).
	// GetGlobalSourceState registers the source's blockable state with the DeltaMultiFileList so the
	// sink can wake it; GetDataInternal converts the multi-file source's transient catch-up (no file
	// yet, list not closed) into SourceResultType::BLOCKED instead of FINISHED.
	//===--------------------------------------------------------------------===//
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	//! Column positions of path / fileConstantValues / deletionVector in the build subplan's output
	//! (the scan_file_row shape). Set in CreatePlan from the bound child's output names. The sink reads
	//! these columns out of each build chunk and ingests them into the DeltaMultiFileList.
	idx_t build_path_col = DConstants::INVALID_INDEX;
	idx_t build_fcv_col = DConstants::INVALID_INDEX;
	idx_t build_dv_col = DConstants::INVALID_INDEX;
};

//! LogicalExtensionOperator returned by delta_scan's bind_operator. Holds the
//! bound table-function state and plans into a PhysicalDeltaLoad.
struct LogicalDeltaGet : public LogicalExtensionOperator {
	LogicalDeltaGet(idx_t bind_index, TableFunction function, unique_ptr<FunctionData> bind_data,
	                vector<LogicalType> returned_types, vector<string> returned_names,
	                virtual_column_map_t virtual_columns, vector<Value> parameters,
	                named_parameter_map_t named_parameters)
	    : bind_index(bind_index), function(std::move(function)), bind_data(std::move(bind_data)),
	      returned_types(std::move(returned_types)), returned_names(std::move(returned_names)),
	      virtual_columns(std::move(virtual_columns)), parameters(std::move(parameters)),
	      named_parameters(std::move(named_parameters)) {
		types = this->returned_types;
	}

	idx_t bind_index;
	TableFunction function;
	unique_ptr<FunctionData> bind_data;
	vector<LogicalType> returned_types;
	vector<string> returned_names;
	virtual_column_map_t virtual_columns;
	vector<Value> parameters;
	named_parameter_map_t named_parameters;

	//! Build subplan (metadata reconciliation) output column positions, set in bind_operator.
	idx_t build_path_col = DConstants::INVALID_INDEX;
	idx_t build_fcv_col = DConstants::INVALID_INDEX;
	idx_t build_dv_col = DConstants::INVALID_INDEX;

	//! M5 (faithful Load): the data-stage kernel LoadNode params as JSON (file_type,
	//! metadata_derived_columns, dv{column,kind}, base_url), emitted over FFI. Empty unless
	//! DELTA_KERNEL_PLAN_LOADNODE is set. Drives the operator by the real IR Load node.
	string load_node_json;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

	void ResolveTypes() override {
		types = returned_types;
		// delta_load: the streaming-input child (the scan_file_row relation, e.g. n7) is attached by the
		// binder AFTER bind_operator returns, so we cannot declare our column dependency there. Do it
		// here (ResolveTypes runs on the full bound tree before optimization): reference every child
		// column so RemoveUnusedColumns keeps the path/fileConstantValues/deletionVector columns the
		// sink consumes at runtime. Idempotent + only when a child is present (delta_scan's P1 path sets
		// expressions in bind_operator, so this is skipped there).
		if (!children.empty() && expressions.empty()) {
			children[0]->ResolveOperatorTypes();
			auto child_bindings = children[0]->GetColumnBindings();
			auto &child_types = children[0]->types;
			for (idx_t i = 0; i < child_bindings.size(); i++) {
				expressions.push_back(make_uniq<BoundColumnRefExpression>(child_types[i], child_bindings[i]));
			}
		}
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return GenerateColumnBindings(bind_index, returned_types.size());
	}

	string GetExtensionName() const override {
		return "delta_scan";
	}

	void Serialize(Serializer &serializer) const override {
		throw NotImplementedException("LogicalDeltaGet serialization not implemented");
	}
};

} // namespace duckdb
