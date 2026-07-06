#include "functions/delta_scan/physical_delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parser.hpp"

#include <cstdlib>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Operator-path filter pushdown (optimizer extension).
//
// The custom LogicalDeltaGet (a LogicalExtensionOperator) is invisible to DuckDB's FilterPushdown,
// which only knows LogicalGet — so the pushed-down WHERE clause lands as a LogicalFilter ABOVE the
// operator and the operator's DeltaMultiFileList keeps EMPTY table_filters (no data-skipping).
//
// This extension runs after DuckDB's optimizers: it finds each LogicalFilter directly over a
// LogicalDeltaGet (delta_scan only), converts those filter expressions into a TableFilterSet with the
// same FilterCombiner the standard path uses, pushes them into the operator's DeltaMultiFileList
// (populating table_filters), and swaps the resulting filtered list into the operator's bind_data. The
// reconciliation subplan then builds its PredicateVisitor from those table_filters (BuildReconciliationSQL),
// so it prunes files exactly like step-1. The LogicalFilter is left in place (it re-checks rows; the
// pushdown only adds stats-based file skipping — never changes results). Only active for the operator path.
//===--------------------------------------------------------------------===//
static void DeltaScanPushdownIntoOperator(LogicalDeltaGet &op, vector<unique_ptr<Expression>> &filter_exprs,
                                          ClientContext &context) {
	// delta_load (faithful Load) sets build columns; its file list is sink-fed, not snapshot-driven, so
	// there is nothing to push a stats predicate into. Only plain delta_scan (no build child) applies.
	if (op.build_path_col != DConstants::INVALID_INDEX || !op.bind_data) {
		return;
	}
	auto *mf_bind = dynamic_cast<MultiFileBindData *>(op.bind_data.get());
	if (!mf_bind || !mf_bind->file_list) {
		return;
	}
	auto *delta_list = dynamic_cast<DeltaMultiFileList *>(mf_bind->file_list.get());
	if (!delta_list) {
		return;
	}

	// Build a MultiFilePushdownInfo for a full scan: column_ids = 0..n-1, column_indexes likewise. The
	// filter expressions reference ColumnBinding(op.bind_index, col), matching GenerateColumnBindings.
	idx_t n = op.returned_types.size();
	vector<ColumnIndex> column_indexes;
	column_indexes.reserve(n);
	for (idx_t i = 0; i < n; i++) {
		column_indexes.emplace_back(i);
	}

	// Convert the filter expressions into a TableFilterSet via the same FilterCombiner the standard path
	// uses (multi_file ComplexFilterPushdown -> DeltaMultiFileList::ComplexFilterPushdown). We call the
	// lower-level PushdownInternal directly (NOT ComplexFilterPushdown) on purpose: ComplexFilterPushdown
	// runs ReportFilterPushdown, which — when delta_scan_explain_files_filtered is on — calls
	// GetTotalFileCount on the new list and EAGERLY populates it via the synchronous kernel iterator. The
	// reconciliation subplan's streaming sink would then append a SECOND time, double-counting files. Going
	// through PushdownInternal keeps the new list empty so the subplan sink is its sole populator.
	FilterCombiner combiner(context);
	for (auto riter = filter_exprs.rbegin(); riter != filter_exprs.rend(); ++riter) {
		combiner.AddFilter((*riter)->Copy());
	}
	vector<FilterPushdownResult> pushdown_results;
	auto filter_set = combiner.GenerateTableScanFilters(column_indexes, pushdown_results);
	if (filter_set.filters.empty()) {
		return; // nothing prunable
	}
	auto new_list = delta_list->PushdownInternal(context, filter_set);
	// The reconciliation subplan feeds the swapped-in list from a streaming sink. Mark it streaming so a
	// bind/plan-time GetCardinality on this list serves resolved_files (empty) instead of triggering the
	// synchronous kernel scan. Mirrors the bind-time mark in DeltaScanBindOperator.
	new_list->MarkStreamingPopulated();
	mf_bind->file_list = shared_ptr<MultiFileList>(std::move(new_list));
}

//===--------------------------------------------------------------------===//
// Reconciliation subplan attachment. Turn the delta_scan operator's metadata reconciliation
// into a REAL bound child subplan so EXPLAIN shows its nodes (READ_JSON per commit, UNION, arg_max dedup,
// tombstone FILTER, and — when a WHERE predicate was pushed down — the data-skipping FILTER over
// add.stats_parsed). Runs post-pushdown, so the dynamic predicate already threaded into the operator's
// DeltaMultiFileList::table_filters is baked into the kernel-lowered SQL. PhysicalDeltaLoad's existing
// streaming build-sink then populates the file list from this subplan's rows (path / fileConstantValues /
// deletionVector), exactly as the delta_load TVF does — no nested Connection.
//===--------------------------------------------------------------------===//
static void DeltaScanAttachReconciliationSubplan(LogicalDeltaGet &op, OptimizerExtensionInput &input) {
	// Only plain delta_scan (no build child yet). delta_load already has its subplan + build columns.
	if (!op.children.empty() || op.build_path_col != DConstants::INVALID_INDEX || !op.bind_data) {
		return;
	}
	auto *mf_bind = dynamic_cast<MultiFileBindData *>(op.bind_data.get());
	if (!mf_bind || !mf_bind->file_list) {
		return;
	}
	auto *delta_list = dynamic_cast<DeltaMultiFileList *>(mf_bind->file_list.get());
	if (!delta_list) {
		return;
	}

	// Lower the kernel metadata-only scan SM to reconciliation SQL WITH the pushed-down predicate baked
	// in (the stats-based file-skip FILTER over add.stats_parsed is present exactly when a WHERE predicate
	// reached table_filters). DuckDB drives the SM (executes each Reduce) inside here.
	string sql = delta_list->BuildReconciliationSQL(input.context);

	// Parse + bind the SQL into a fully-planned child LogicalOperator. The child binder shares the query's
	// GlobalBinderState (bound_tables counter) with the main plan's binder, so it allocates FRESH table
	// indices past every main-plan index — no binding collisions. Done post-pushdown so the predicate is
	// known.
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw IOException("delta_scan: expected a single SELECT statement from reconciliation lowering");
	}
	auto child_binder = Binder::CreateBinder(input.context, &input.optimizer.binder);
	auto bound = child_binder->Bind(*parser.statements[0]);
	if (!bound.plan) {
		throw IOException("delta_scan: failed to bind reconciliation subplan");
	}

	// The metadata-only terminal emits columns path / fileConstantValues / deletionVector (+ size). Map
	// their positions in the subplan's output so PhysicalDeltaLoad's sink ingests each row.
	for (idx_t c = 0; c < bound.names.size(); c++) {
		const auto &nm = bound.names[c];
		if (nm == "path") {
			op.build_path_col = c;
		} else if (nm == "fileConstantValues") {
			op.build_fcv_col = c;
		} else if (nm == "deletionVector") {
			op.build_dv_col = c;
		}
	}
	if (op.build_path_col == DConstants::INVALID_INDEX) {
		throw IOException("delta_scan: reconciliation subplan is missing the 'path' column");
	}

	// Attach as the operator's build child. CreatePlan plans it as the build/sink pipeline; BuildPipelines
	// runs it as a streaming child meta pipeline feeding the source. Attached AFTER all optimizers, so
	// RemoveUnusedColumns never prunes the subplan's terminal columns (the sink reads them by position).
	op.children.push_back(std::move(bound.plan));
}

static void DeltaScanOptimizeFilterPushdown(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	// Walk the plan: for each LogicalFilter directly over a delta LogicalDeltaGet, push its predicates
	// into the operator's file list (populating table_filters — the dynamic data-skipping predicate).
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER && !plan->children.empty() &&
	    plan->children[0]->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = plan->children[0]->Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "delta_scan") {
			DeltaScanPushdownIntoOperator(ext.Cast<LogicalDeltaGet>(), plan->expressions, input.context);
		}
	}
	// Attach the reconciliation subplan to EVERY delta_scan operator (with or without a WHERE above it).
	// Done when we directly reach the operator in the walk — after the parent filter (if any) has already
	// pushed its predicate into the file list above — so the subplan SQL reflects it. Every delta_scan's
	// file list was marked streaming (at bind / on pushdown), so it MUST get a sink: attaching the subplan
	// unconditionally is what populates it (a streaming list with no sink would hang).
	if (plan->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = plan->Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "delta_scan") {
			DeltaScanAttachReconciliationSubplan(ext.Cast<LogicalDeltaGet>(), input);
		}
	}
	for (auto &child : plan->children) {
		DeltaScanOptimizeFilterPushdown(input, child);
	}
}

void RegisterDeltaScanOptimizer(DBConfig &config) {
	OptimizerExtension ext;
	ext.optimize_function = DeltaScanOptimizeFilterPushdown;
	OptimizerExtension::Register(config, ext);
}

PhysicalOperator &LogicalDeltaGet::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	// M0: scan all returned columns, no pushed filters (projection/filter pushdown into the custom
	// operator is a later milestone — Q4). The file list is still resolved lazily inside
	// DeltaMultiFileList, so behavior matches the current PhysicalTableScan path exactly; only the
	// operator class changes.
	vector<ColumnIndex> column_ids;
	column_ids.reserve(returned_types.size());
	for (idx_t i = 0; i < returned_types.size(); i++) {
		column_ids.emplace_back(i);
	}

	vector<LogicalType> output_types = returned_types;
	vector<LogicalType> all_types = returned_types;
	vector<string> names = returned_names;
	auto params = parameters;
	auto vcols = virtual_columns;

	auto &scan = planner.Make<PhysicalDeltaLoad>(
	    std::move(output_types), function, std::move(bind_data), std::move(all_types), std::move(column_ids),
	    vector<idx_t>(), std::move(names), unique_ptr<TableFilterSet>(), estimated_cardinality, ExtraOperatorInfo(),
	    std::move(params), std::move(vcols));

	// M1: if a metadata-reconciliation subplan was attached at bind time, plan it as the build child
	// and forward the scan_file_row column positions so the sink can ingest them.
	if (!children.empty()) {
		auto &child_plan = planner.CreatePlan(*children[0]);
		scan.children.push_back(child_plan);
		auto &delta_scan = scan.Cast<PhysicalDeltaLoad>();
		delta_scan.build_path_col = build_path_col;
		delta_scan.build_fcv_col = build_fcv_col;
		delta_scan.build_dv_col = build_dv_col;
	}
	return scan;
}

// Reach the DeltaMultiFileList that the source side reads, via the operator's bind data.
static DeltaMultiFileList &GetDeltaFileList(FunctionData &bind_data) {
	auto &mf_bind_data = bind_data.Cast<MultiFileBindData>();
	return mf_bind_data.file_list->Cast<DeltaMultiFileList>();
}

//===--------------------------------------------------------------------===//
// Build sink (M1.1): consume the metadata-reconciliation subplan's scan_file_rows and build the
// surviving file list directly into the DeltaMultiFileList that the source side reads. This is the
// faithful realization of the kernel data-stage Load's input: the build pipeline produces the
// scan_file_rows; the sink ingests each (path, fileConstantValues, deletionVector) row.
//===--------------------------------------------------------------------===//
class DeltaScanBuildGlobalState : public GlobalSinkState {};

// Local sink state. In streaming mode each Sink() chunk publishes its resolved entries to the shared
// list immediately (so the concurrent source sees files as they arrive), so this holds only a small
// reusable scratch batch — DV resolution still runs lock-free in BuildFileEntry before the locked
// append.
class DeltaScanBuildLocalState : public LocalSinkState {
public:
	vector<DeltaMultiFileList::ResolvedFileEntry> entries;
};

unique_ptr<GlobalSinkState> PhysicalDeltaLoad::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DeltaScanBuildGlobalState>();
}

unique_ptr<LocalSinkState> PhysicalDeltaLoad::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<DeltaScanBuildLocalState>();
}

SinkResultType PhysicalDeltaLoad::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<DeltaScanBuildLocalState>();
	auto &file_list = GetDeltaFileList(*bind_data);
	chunk.Flatten();
	lstate.entries.clear();
	for (idx_t r = 0; r < chunk.size(); r++) {
		Value path_val = chunk.GetValue(build_path_col, r);
		Value fcv_val = build_fcv_col != DConstants::INVALID_INDEX ? chunk.GetValue(build_fcv_col, r) : Value();
		Value dv_val = build_dv_col != DConstants::INVALID_INDEX ? chunk.GetValue(build_dv_col, r) : Value();
		// Lock-free build (resolves the DV in parallel across sink threads).
		auto entry = file_list.BuildFileEntry(path_val, fcv_val, dv_val);
		if (!entry.file.path.empty()) {
			lstate.entries.push_back(std::move(entry));
		}
	}
	// Publish this chunk's files to the shared list NOW (under one lock) and wake the concurrent
	// source — streaming: the source can start scanning these files before the rest of the input feed
	// has been consumed.
	file_list.AppendResolvedEntries(std::move(lstate.entries));
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDeltaLoad::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	// Nothing to flush — Sink publishes each chunk immediately for streaming. (Kept for the sink API.)
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalDeltaLoad::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                             OperatorSinkFinalizeInput &input) const {
	// The input feed is fully consumed: close the listing (no more files) and wake any source blocked
	// at the tail so it observes closed-and-drained and finishes.
	GetDeltaFileList(*bind_data).MarkExternallyPopulated();
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Streaming source side
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSourceState> PhysicalDeltaLoad::GetGlobalSourceState(ClientContext &context) const {
	auto state = PhysicalTableScan::GetGlobalSourceState(context);
	auto &file_list = GetDeltaFileList(*bind_data);
	// Register the source's blockable state so the sink can wake it on append/close. GlobalSourceState
	// publicly derives StateWithBlockableTasks.
	file_list.SetBlockedSourceState(state.get());
	return state;
}

SourceResultType PhysicalDeltaLoad::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                    OperatorSourceInput &input) const {
	auto result = PhysicalTableScan::GetDataInternal(context, chunk, input);
	if (result != SourceResultType::FINISHED || chunk.size() != 0) {
		return result;
	}
	// FINISHED with no rows. If the streaming listing is still open, this is a transient catch-up to the
	// tail (the multi-file source kept the scan resumable via blocked_on_growing_list), NOT true EOF.
	// Block until the sink appends more files or closes the listing. input.global_state is a
	// GlobalSourceState, which publicly derives StateWithBlockableTasks (Lock/BlockSource).
	auto &file_list = GetDeltaFileList(*bind_data);
	if (file_list.IsFinalizedListing()) {
		return SourceResultType::FINISHED;
	}
	// Caught up to the tail of a still-open streaming listing. Acquire the source-state lock and
	// RE-CHECK closed-ness under it before blocking. WakeBlockedSource (append + the final close-wake in
	// Finalize) takes this SAME lock, so any close that lands after the first check above is either
	// observed here (return FINISHED, no block) or its wake is delivered to our now-registered block —
	// never lost. Without this re-check, a close firing between the first check and BlockSource would
	// wake zero registered tasks and strand this source forever (every worker thread ends up parked in
	// futex_wait at 0% CPU). This is the lost-wakeup that deadlocked multi-delta_scan queries.
	auto guard = input.global_state.Lock();
	if (file_list.IsFinalizedListing()) {
		return SourceResultType::FINISHED;
	}
	return input.global_state.BlockSource(guard, input.interrupt_state);
}

//===--------------------------------------------------------------------===//
// Pipeline construction: build (metadata subplan) -> sink(this); probe (data scan) = source(this).
// Mirrors PhysicalCTE / the join-build pattern. Falls back to a plain source when there is no child.
//===--------------------------------------------------------------------===//
void PhysicalDeltaLoad::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();

	auto &state = meta_pipeline.GetState();
	// This operator is the source of the current (probe) pipeline.
	state.SetPipelineSource(current, *this);
	if (!children.empty()) {
		// Enter streaming mode: the source serves resolved_files directly and reports the listing as
		// non-finalized (so it BLOCKs at the tail) until the sink's Finalize closes it.
		GetDeltaFileList(*bind_data).MarkStreamingPopulated();
		// The input feed (scan_file_row producer) sinks into this operator. Use the STREAMING child
		// meta pipeline so the feed runs CONCURRENTLY with the source side — the source hands out
		// row-group morsels of files already resolved while the feed keeps appending more, and blocks
		// (SourceResultType::BLOCKED) only when momentarily caught up. No build-before-probe barrier.
		auto &child_meta_pipeline = meta_pipeline.CreateStreamingChildMetaPipeline(current, *this);
		child_meta_pipeline.Build(children[0]);
	}
}

} // namespace duckdb
