#include "functions/delta_scan/physical_delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"
#include "functions/delta_scan/delta_scan_sm_driver.hpp"

#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parser.hpp"

#include <cstdlib>

namespace duckdb {

//! Async metadata-scan SM mode (Option B / Tier-2): drive the kernel scan SM at execution time as
//! scheduler work so the source yields its worker per Reduce instead of pinning it for the whole
//! reconciliation. Gated behind DELTA_KERNEL_PLAN_SM_ASYNC; the synchronous DELTA_KERNEL_PLAN_SM path
//! and the default (legacy) path are untouched.
static bool AsyncScanSMEnabled() {
	return std::getenv("DELTA_KERNEL_PLAN_SM_ASYNC") != nullptr;
}

//! Synchronous metadata-scan SM mode rendered as a VISIBLE child subplan (this feature): the metadata
//! reconciliation (READ_JSON per commit, UNION, arg_max dedup, tombstone + data-skipping stats FILTER)
//! is attached as a real bound child of the delta_scan operator so it appears in EXPLAIN, instead of
//! running inside an opaque nested Connection. Gated behind DELTA_KERNEL_PLAN_SM.
static bool SyncScanSMEnabled() {
	return std::getenv("DELTA_KERNEL_PLAN_SM") != nullptr;
}

//===--------------------------------------------------------------------===//
// Operator-path filter pushdown (optimizer extension).
//
// The custom LogicalDeltaGet (a LogicalExtensionOperator) is invisible to DuckDB's FilterPushdown,
// which only knows LogicalGet — so the pushed-down WHERE clause lands as a LogicalFilter ABOVE the
// operator and the operator's DeltaMultiFileList keeps EMPTY table_filters (no data-skipping). The
// synchronous DELTA_KERNEL_PLAN_SM / default paths use a real LogicalGet and don't have this problem.
//
// This extension runs after DuckDB's optimizers: it finds each LogicalFilter directly over a
// LogicalDeltaGet (delta_scan only), converts those filter expressions into a TableFilterSet with the
// same FilterCombiner the standard path uses, pushes them into the operator's DeltaMultiFileList
// (populating table_filters), and swaps the resulting filtered list into the operator's bind_data. The
// async SM driver then builds its PredicateVisitor from those table_filters (OpenScanSMForAsync), so it
// prunes files exactly like step-1. The LogicalFilter is left in place (it re-checks rows; the pushdown
// only adds stats-based file skipping — never changes results). Only active for the operator path.
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
	// execution-time async SM driver would then append a SECOND time, double-counting files. Going through
	// PushdownInternal keeps the new list empty so the async SM is its sole populator.
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
	// In async mode the swapped-in list replaces the bind-time-marked one. Mark it streaming so a
	// bind/plan-time GetCardinality on this list serves resolved_files (empty) instead of triggering the
	// synchronous kernel scan. Mirrors the bind-time mark in DeltaScanBindOperator. The SM-visible-subplan
	// path also feeds the list from a streaming sink, so it needs the same mark on the swapped-in list.
	if (AsyncScanSMEnabled() || SyncScanSMEnabled()) {
		new_list->MarkStreamingPopulated();
	}
	mf_bind->file_list = shared_ptr<MultiFileList>(std::move(new_list));
}

//===--------------------------------------------------------------------===//
// SM visible-subplan attachment (this feature). Turn the delta_scan operator's metadata reconciliation
// into a REAL bound child subplan so EXPLAIN shows its nodes (READ_JSON per commit, UNION, arg_max dedup,
// tombstone FILTER, and — when a WHERE predicate was pushed down — the data-skipping FILTER over
// add.stats_parsed). Runs post-pushdown, so the dynamic predicate already threaded into the operator's
// DeltaMultiFileList::table_filters is baked into the kernel-lowered SQL. PhysicalDeltaLoad's existing
// streaming build-sink then populates the file list from this subplan's rows (path / fileConstantValues /
// deletionVector), exactly as the delta_load TVF and async paths do — no nested Connection.
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
	// indices past every main-plan index — no binding collisions. This is the SAME mechanism the P2
	// bind_replace subquery path uses; here we do it post-pushdown so the predicate is known.
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw IOException("DELTA_KERNEL_PLAN_SM: expected a single SELECT statement from reconciliation lowering");
	}
	auto child_binder = Binder::CreateBinder(input.context, &input.optimizer.binder);
	auto bound = child_binder->Bind(*parser.statements[0]);
	if (!bound.plan) {
		throw IOException("DELTA_KERNEL_PLAN_SM: failed to bind reconciliation subplan");
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
		throw IOException("DELTA_KERNEL_PLAN_SM: reconciliation subplan is missing the 'path' column");
	}

	// Attach as the operator's build child. CreatePlan plans it as the build/sink pipeline; BuildPipelines
	// runs it as a streaming child meta pipeline feeding the source. Attached AFTER all optimizers, so
	// RemoveUnusedColumns never prunes the subplan's terminal columns (the sink reads them by position).
	op.children.push_back(std::move(bound.plan));
}

static void DeltaScanOptimizeFilterPushdown(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (!AsyncScanSMEnabled() && !SyncScanSMEnabled() && !std::getenv("DELTA_KERNEL_PLAN_OP")) {
		return; // only the operator path needs this; leave every other query untouched
	}
	// Walk the plan: for each LogicalFilter directly over a delta LogicalDeltaGet, push its predicates
	// into the operator's file list (populating table_filters — the dynamic data-skipping predicate).
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER && !plan->children.empty() &&
	    plan->children[0]->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = plan->children[0]->Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "delta_scan") {
			DeltaScanPushdownIntoOperator(ext.Cast<LogicalDeltaGet>(), plan->expressions, input.context);
		}
	}
	// SM visible-subplan: attach the reconciliation subplan to EVERY delta_scan operator (with or without a
	// WHERE above it). Done when we directly reach the operator in the walk — after the parent filter (if
	// any) has already pushed its predicate into the file list above — so the subplan SQL reflects it.
	// Every SM delta_scan's file list was marked streaming (at bind / on pushdown), so it MUST get a sink:
	// attaching the subplan unconditionally is what populates it (a streaming list with no sink would hang).
	if (SyncScanSMEnabled() && plan->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
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
// Async metadata-scan SM driver (TRUE per-request Option B). Drives the kernel scan SM at execution time
// under a single-driver election: exactly ONE worker (the elected driver) advances the single-owner SM
// cursor; every other worker returns SourceResultType::BLOCKED and waits on the "list populated/closed"
// wake (the DeltaMultiFileList streaming channel the sink uses), then serves files.
//
// Per-reduce yielding: the elected driver does NOT pump the SM inline on its worker thread. On each
// KDF_STEP_REDUCE it dispatches the reduce SQL as scheduler work (a Task on the QUERY executor's own
// producer token, so threads=1/0 still drain it) and RETURNS BLOCKED, releasing its worker thread for
// other pipelines. When the reduce task finishes it parks the Arrow result in the shared ReduceChannel
// and fires the driver's stored InterruptState. The woken driver re-enters, submits the result, and
// pulls the next step. The continuation is single-elected (channel state READY -> CONSUMED under `mu`),
// so a spuriously-woken second worker re-BLOCKs instead of double-submitting.
//
// Lifetime-safe wake (the crux — the use-after-free the previous inline version was avoiding):
//   * The driver parks via DuckDB's TASK-mode InterruptState (a weak_ptr<Task>), captured BY VALUE — not
//     a raw GlobalSourceState*. If the query tore down and freed the blocked pipeline task,
//     InterruptState::Callback() locks an expired weak_ptr and is a no-op.
//   * The reduce task touches ONLY the heap ReduceChannel, co-owned via shared_ptr. If the driver / list
//     / bind_data is destroyed mid-flight (a sibling scan threw, query unwinding), the driver's strong
//     ref drops but the task keeps the channel alive: the task writes its result into the live channel
//     and fires the (no-op) wake, then drops its ref and the channel frees. A reduce firing after the
//     source/driver are gone is a guaranteed no-op, never a deref of freed memory.
//===--------------------------------------------------------------------===//

//! Off-thread reduce: runs one kernel Reduce's SQL -> Arrow on the query's DatabaseInstance and parks the
//! result in the shared ReduceChannel, then wakes the parked driver. Co-owns the channel (so it can never
//! write to freed memory) and the DatabaseInstance (so the DB outlives the off-thread query). Runs once to
//! completion (TASK_FINISHED); it is never rescheduled — the thing that gets rescheduled is the driver's
//! own blocked pipeline task, via channel->wake.
class DeltaReduceTask : public Task {
public:
	DeltaReduceTask(shared_ptr<ReduceChannel> channel, shared_ptr<DatabaseInstance> db, string sql)
	    : channel(std::move(channel)), db(std::move(db)), sql(std::move(sql)) {
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		ffi::FFI_ArrowArray array {};
		ffi::FFI_ArrowSchema schema {};
		string err;
		bool ok = false;
		try {
			ok = delta_sdk::RunSqlToArrowOnDB(*db, sql, &array, &schema, err);
		} catch (std::exception &ex) {
			ok = false;
			err = ex.what();
		} catch (...) {
			ok = false;
			err = "unknown error executing reduce SQL";
		}
		InterruptState wake;
		{
			lock_guard<mutex> l(channel->mu);
			if (ok) {
				channel->array = array;
				channel->schema = schema;
				channel->has_result = true;
				channel->state = ReduceState::READY;
			} else {
				channel->error = err.empty() ? string("failed to execute reduce SQL in DuckDB") : err;
				channel->state = ReduceState::FAILED;
			}
			wake = channel->wake; // copy by value: firing it after teardown is a no-op
		}
		// Wake the parked driver OUTSIDE the channel lock. Callback() on a TASK-mode InterruptState whose
		// task was freed during query teardown is a no-op (expired weak_ptr) — never a UAF.
		wake.Callback();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "DeltaReduceTask";
	}

private:
	shared_ptr<ReduceChannel> channel;
	shared_ptr<DatabaseInstance> db;
	string sql;
};

//! Dispatch the pending reduce as scheduler work on the QUERY executor's own producer token, then park.
//! Caller holds `drive_lock` (driver.mu) and the SM is at a KDF_STEP_REDUCE. We fetch the reduce SQL,
//! arm the channel (state RUNNING, store the by-value wake), schedule the reduce Task, and return — the
//! caller then returns BLOCKED to release the worker. threads=1/0 still drain the task because it lands on
//! the query's own producer queue that the single executor loop services.
static void DispatchReduce(ExecutionContext &context, DeltaScanSMDriver &driver, OperatorSourceInput &input) {
	string reduce_sql = delta_sdk::SmReduceSql(driver.sm);
	if (!driver.reduce) {
		driver.reduce = make_shared_ptr<ReduceChannel>();
	}
	auto &channel = driver.reduce;
	{
		lock_guard<mutex> l(channel->mu);
		channel->ReleaseResult();
		channel->error.clear();
		channel->wake = input.interrupt_state; // by value — the lifetime-safe wake target
		channel->state = ReduceState::RUNNING;
	}
	auto db = context.client.db; // keep the DB alive for the off-thread query
	auto task = make_shared_ptr<DeltaReduceTask>(channel, std::move(db), std::move(reduce_sql));
	auto &executor = context.pipeline->executor;
	auto &scheduler = TaskScheduler::GetScheduler(context.client);
	scheduler.ScheduleTask(executor.GetToken(), std::move(task));
}

//! On a failed drive: record the error, flip FAILED, and wake every blocked worker so each re-enters,
//! sees FAILED, and re-throws the same message (a clean unwind, no worker stranded). The wake must not
//! throw out of here, so swallow any wake error. Caller holds `drive_lock`; we unlock before waking
//! (lock order: never hold `mu` across the source-state lock taken by WakeBlockedSource).
static void FailDrive(DeltaScanSMDriver &driver, DeltaMultiFileList &file_list, unique_lock<mutex> &drive_lock,
                      const string &message) {
	driver.error = message;
	driver.drive = SMDriveState::FAILED;
	drive_lock.unlock();
	try {
		file_list.WakeBlockedSourceFromTask();
	} catch (...) {
	}
}

//! Step the SM forward as far as it can go WITHOUT blocking, holding `drive_lock`. Returns:
//!   * true             -> the SM reached Done: the list is populated/closed, drive == DONE, serve files.
//!   * false (parked)   -> a reduce was dispatched (drive stays DRIVING, channel == RUNNING); the caller
//!                         must return BLOCKED. On the next entry, if the channel is READY we submit the
//!                         result and continue stepping; if FAILED we throw.
//! Throws on a hard SM/finalize error (after marking FAILED + waking blocked workers).
//! This is the re-entrant heart of per-request Option B: each call advances the SM until it next needs an
//! off-thread reduce, then yields the worker.
static bool StepDriveUntilBlockOrDone(ExecutionContext &context, DeltaScanSMDriver &driver,
                                      DeltaMultiFileList &file_list, OperatorSourceInput &input,
                                      unique_lock<mutex> &drive_lock) {
	try {
		if (!driver.opened) {
			driver.sm = file_list.OpenScanSMForAsync();
			driver.opened = true;
		}
		// If a previously-dispatched reduce has completed, consume its result before stepping again.
		if (driver.reduce && driver.reduce->state.load() == ReduceState::READY) {
			auto &channel = *driver.reduce;
			lock_guard<mutex> l(channel.mu);
			delta_sdk::SmSubmitReduce(driver.sm, &channel.array, &channel.schema); // kernel takes ownership
			channel.has_result = false; // ownership transferred — dtor must not release it
			channel.state = ReduceState::CONSUMED;
		}
		for (;;) {
			int32_t kind = delta_sdk::SmGetStep(driver.sm);
			if (kind == ffi::KDF_STEP_DONE) {
				break;
			}
			// KDF_STEP_REDUCE: dispatch it off-thread and park (yield the worker).
			DispatchReduce(context, driver, input);
			return false;
		}
		// Done: lower + run the terminal ResultPlan, append surviving files, close the listing (wakes every
		// blocked source). Only the elected driver reaches here; idempotent for this scan.
		file_list.FinalizeScanSMResult(driver.sm);
		driver.drive = SMDriveState::DONE;
		return true;
	} catch (std::exception &ex) {
		FailDrive(driver, file_list, drive_lock, ex.what());
		throw;
	}
}

//! Advance the async SM under the single-driver election. Returns true if the file list is now fully
//! populated (the caller should serve files); returns false after BLOCKing this worker, in which case
//! `out_result` holds BLOCKED (or FINISHED if blocking is disallowed).
//!
//! Single-driver election (the fix for the intra-query data race): DuckDB runs GetDataInternal on MANY
//! workers concurrently. Exactly ONE may advance the single-owner SM cursor. We elect it with
//! `driver.drive` (NOT_STARTED -> DRIVING -> DONE | FAILED), guarded by `driver.mu`:
//!   * DONE                   -> serve files.
//!   * FAILED                 -> re-throw the recorded error.
//!   * DRIVING                 -> we are either the parked DRIVER coming back to consume a finished reduce,
//!                              OR a bystander worker. The continuation is single-elected by the channel
//!                              state: if a reduce is READY (and not yet CONSUMED) we re-claim the drive and
//!                              step forward; otherwise (RUNNING / CONSUMED / IDLE — not ours) we BLOCK as a
//!                              bystander on the populate/close channel.
//!   * NOT_STARTED            -> claim it and step the SM until it next needs a reduce (-> yield) or Done.
//!
//! TWO distinct blocking channels, deliberately kept separate (this is the fix for the threads=1 self-wake
//! deadlock): the DRIVER, when it dispatches a reduce, parks by returning BLOCKED with its wake stored ONLY
//! in the ReduceChannel (NOT registered in the source-state's blocked_tasks). The reduce task is then the
//! SOLE waker of the driver, via that stored InterruptState. Bystanders, by contrast, register in
//! blocked_tasks (BlockSource) and are woken collectively by the finalize/append WakeBlockedSource. If the
//! driver also registered in blocked_tasks, the finalize's UnblockTasks — which the driver itself runs
//! while NOT blocked — would try to Reschedule the running driver task and spin forever.
static bool DriveAsyncSMTurn(ExecutionContext &context, DeltaScanSMDriver &driver, DeltaMultiFileList &file_list,
                             OperatorSourceInput &input, SourceResultType &out_result) {
	unique_lock<mutex> drive_lock(driver.mu);
	for (;;) {
		switch (driver.drive.load()) {
		case SMDriveState::DONE:
			return true; // list populated/closed: serve files.
		case SMDriveState::FAILED:
			throw IOException("DELTA_KERNEL_PLAN_SM_ASYNC: scan state machine failed: %s", driver.error);
		case SMDriveState::DRIVING: {
			// A drive is in progress. Under `mu` (so the continuation is single-elected) decide our role.
			auto rstate = driver.reduce ? driver.reduce->state.load() : ReduceState::IDLE;
			if (rstate == ReduceState::FAILED) {
				FailDrive(driver, file_list, drive_lock,
				          "DELTA_KERNEL_PLAN_SM_ASYNC: reduce failed: " + driver.reduce->error);
				throw IOException("DELTA_KERNEL_PLAN_SM_ASYNC: scan state machine failed: %s", driver.error);
			}
			if (rstate == ReduceState::READY) {
				// We are the woken driver (single continuation): consume the result and step on. Submitting
				// flips the channel to CONSUMED under `mu`, so a second woken worker arriving here sees
				// CONSUMED (falls into the bystander block) — never a double-submit. If StepDrive yields again
				// (next reduce), it re-armed the channel wake and we return BLOCKED on THAT (no blocked_tasks).
				if (StepDriveUntilBlockOrDone(context, driver, file_list, input, drive_lock)) {
					return true; // reached Done.
				}
				out_result = SourceResultType::BLOCKED; // parked on the re-armed ReduceChannel wake.
				return false;
			}
			// RUNNING / CONSUMED / IDLE and not ours to advance: bystander BLOCK on the populate/close wake.
			// Re-check under the SOURCE-state lock (lost-wakeup discipline) before blocking: WakeBlockedSource
			// takes this same lock, so a close landing between the check and BlockSource is either observed
			// here (loop again) or delivered to our registered block.
			drive_lock.unlock();
			auto guard = input.global_state.Lock();
			auto st = driver.drive.load();
			if (st == SMDriveState::DONE || st == SMDriveState::FAILED || file_list.IsFinalizedListing()) {
				guard.unlock();
				drive_lock.lock();
				continue; // drive finished/failed: re-evaluate (serve files / throw).
			}
			out_result = input.global_state.BlockSource(guard, input.interrupt_state);
			return false;
		}
		case SMDriveState::NOT_STARTED:
			// Claim the drive (single-elected) and step the SM until it needs a reduce (-> yield) or is Done.
			driver.drive = SMDriveState::DRIVING;
			if (StepDriveUntilBlockOrDone(context, driver, file_list, input, drive_lock)) {
				return true; // reached Done in one go (no reduces).
			}
			// Dispatched the first reduce and parked: return BLOCKED on the ReduceChannel wake (armed by
			// DispatchReduce). The reduce task is the sole waker; we are NOT in blocked_tasks.
			out_result = SourceResultType::BLOCKED;
			return false;
		}
	}
}

//===--------------------------------------------------------------------===//
// Streaming source side
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSourceState> PhysicalDeltaLoad::GetGlobalSourceState(ClientContext &context) const {
	auto state = PhysicalTableScan::GetGlobalSourceState(context);
	auto &file_list = GetDeltaFileList(*bind_data);
	// Register the source's blockable state so the sink (or the async SM driver) can wake it on
	// append/close. GlobalSourceState publicly derives StateWithBlockableTasks.
	file_list.SetBlockedSourceState(state.get());
	// Async SM mode (no data-child, plain delta_scan): mark the listing streaming so GetFileInternal
	// serves resolved_files directly (empty until the SM finalizes) and the source BLOCKs at the tail
	// instead of falling into the synchronous kernel scan path. The driver closes it via FinalizeScanSMResult.
	if (AsyncScanSMEnabled() && children.empty()) {
		file_list.MarkStreamingPopulated();
	}
	return state;
}

SourceResultType PhysicalDeltaLoad::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                    OperatorSourceInput &input) const {
	// Async SM mode: drive the kernel scan SM (as scheduler work) until the file list is populated, then
	// fall through to the normal parallel multi-file scan. Only for plain delta_scan (no data child).
	if (AsyncScanSMEnabled() && children.empty()) {
		auto &file_list = GetDeltaFileList(*bind_data);
		if (!file_list.IsFinalizedListing()) {
			auto &driver = file_list.GetOrCreateSMDriver(context.client);
			// Single-driver election: exactly one worker drives the SM inline to completion; the rest BLOCK
			// until the list is populated/closed, then serve files. Correct at any thread count (incl.
			// threads=1: the elected worker drives synchronously and never waits on another task).
			SourceResultType blocked_result;
			if (!DriveAsyncSMTurn(context, driver, file_list, input, blocked_result)) {
				return blocked_result;
			}
			// Fall through: list is populated, serve files below.
		}
	}
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
