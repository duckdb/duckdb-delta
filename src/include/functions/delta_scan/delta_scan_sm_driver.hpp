//===----------------------------------------------------------------------===//
// delta::DeltaScanSMDriver — Option B / Tier-2 async driver for the metadata scan SM.
//
// The kernel CoroutineSM is a passive, single-owner, `!Send` synchronous stepper. We drive it at
// EXECUTION time from the data-source operator (PhysicalDeltaLoad::GetDataInternal). Under DuckDB
// intra-query parallelism the operator runs on MANY worker threads, so a single-driver election
// (`DeltaScanSMDriver::drive`) picks exactly ONE worker to advance the SM; every other worker returns
// SourceResultType::BLOCKED and waits on the DeltaMultiFileList "list populated/closed" wake (the same
// streaming channel the sink uses), then serves files.
//
// TRUE per-request Option B (per-reduce yielding): the elected driver does NOT pump the SM inline. On
// each kernel KDF_STEP_REDUCE it dispatches the reduce SQL as scheduler work (a Task on the query
// executor's own producer token — so threads=1/0 still make progress) and RETURNS BLOCKED, releasing
// its worker thread. When the reduce task finishes it parks the Arrow result in a shared, ref-counted
// `ReduceChannel` and fires a stored InterruptState to wake the parked driver. Exactly one woken worker
// re-claims the continuation (`reduce` flips RUNNING->READY->CONSUMED under `mu`), submits the result,
// and pulls the next step. On KDF_STEP_DONE it populates the list + closes.
//
// Lifetime-safe wake (the crux — this is the use-after-free the inline fallback was avoiding):
//   * The wake is delivered through a stored `InterruptState` (TASK mode = a `weak_ptr<Task>`), captured
//     by value when the driver parks — NEVER a raw GlobalSourceState*. If the query tore down and freed
//     the blocked pipeline task, `InterruptState::Callback()` locks an expired weak_ptr and is a no-op.
//   * The reduce task touches ONLY the heap `ReduceChannel`, which it co-owns via a `shared_ptr`. If the
//     driver / DeltaMultiFileList / bind_data is destroyed mid-flight (e.g. a sibling scan threw and the
//     query is unwinding), the driver's strong ref drops but the task's strong ref keeps the channel
//     alive; the task writes its result into the still-live channel and fires the (now no-op) wake, then
//     drops its ref and the channel frees. A reduce task firing after the source/driver are gone is a
//     guaranteed no-op, never a dereference of freed memory.
//
// Threading model: the KdfSM* is a single-owner cursor guarded by `mu`. Every kdf_sm_* call happens
// while `mu` is held, and the per-reduce continuation is single-elected, so the kernel's non-atomic Rc
// refcount ops are never raced and we never call two kdf_sm_* entry points concurrently.
//===----------------------------------------------------------------------===//
#pragma once

#include "functions/delta_scan/sm_sdk.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/parallel/interrupt.hpp"

namespace duckdb {

class DeltaMultiFileList;

//! Single-driver election state for the SM drive. Under intra-query parallelism DuckDB creates one local
//! source state per worker and each runs GetDataInternal, so MANY workers reach the driver concurrently.
//! The SM is a single-owner cursor: exactly ONE worker may drive it; every other worker must NOT start a
//! parallel drive — it returns BLOCKED and waits on the "list populated/closed" wake, then serves files.
//! Atomic so a worker can read it under the source-state lock (lost-wakeup discipline) without taking
//! `mu`; transitions happen under `mu`.
enum class SMDriveState : uint8_t {
	NOT_STARTED, //! no worker has claimed the drive yet
	DRIVING,     //! the drive is in progress (the elected driver is parked between reduces, or stepping)
	DONE,        //! the drive finished; the list is populated/closed — all workers serve files
	FAILED       //! the drive threw; `error` holds the message — every worker re-throws it
};

//! Per-reduce continuation state. While a reduce runs off-thread the driver parks (returns BLOCKED). The
//! reduce task flips RUNNING -> READY (or FAILED) and wakes the driver. Exactly one woken worker re-claims
//! the continuation: it transitions READY -> CONSUMED under `mu`, so a second woken worker observes
//! CONSUMED (not READY) and re-BLOCKs instead of double-submitting.
enum class ReduceState : uint8_t {
	IDLE,     //! no reduce in flight; the driver is free to step the SM
	RUNNING,  //! a reduce task is computing the Arrow result off-thread; the driver is parked
	READY,    //! the reduce result is parked in the channel, ready for the driver to submit
	CONSUMED, //! a woken worker has claimed the result; back to stepping
	FAILED    //! the reduce SQL failed; `error` (in the channel) holds the message
};

//! Heap, ref-counted hand-off between a reduce Task (scheduler thread) and the parked driver. Co-owned by
//! BOTH: a reduce firing after the driver/list/bind_data is gone writes into this still-live channel and
//! its wake is a no-op — so it can never dereference freed driver memory. The Arrow out-params are owned
//! here until consumed; the destructor releases any unconsumed array/schema (the kernel never took them).
struct ReduceChannel {
	mutex mu;
	atomic<ReduceState> state {ReduceState::IDLE};
	//! Exported reduce result (ownership transfers to the kernel on submit; released in dtor if unconsumed).
	ffi::FFI_ArrowArray array {};
	ffi::FFI_ArrowSchema schema {};
	bool has_result = false; //! true once `array`/`schema` hold a not-yet-submitted batch
	string error;            //! set when state == FAILED
	//! Wake target for the parked driver, stored BY VALUE (TASK mode = weak_ptr<Task>): a no-op if the
	//! blocked pipeline task was freed during query teardown. Set under `mu` each time the driver parks.
	InterruptState wake;

	~ReduceChannel() {
		ReleaseResult();
	}
	//! Release an exported-but-unsubmitted Arrow batch (kernel takes ownership only via SmSubmitReduce).
	void ReleaseResult() {
		if (has_result) {
			auto *arr = reinterpret_cast<ArrowArray *>(&array);
			auto *sch = reinterpret_cast<ArrowSchema *>(&schema);
			if (arr->release) {
				arr->release(arr);
			}
			if (sch->release) {
				sch->release(sch);
			}
			has_result = false;
		}
	}
};

//! Per-scan async SM driver, owned by the DeltaMultiFileList (one per delta_scan). Created lazily the
//! first time the async-SM source runs.
class DeltaScanSMDriver {
public:
	DeltaScanSMDriver() = default;
	~DeltaScanSMDriver() {
		delta_sdk::SmFree(sm);
	}
	DeltaScanSMDriver(const DeltaScanSMDriver &) = delete;
	DeltaScanSMDriver &operator=(const DeltaScanSMDriver &) = delete;

	//! The per-SM cursor mutex. Every kdf_sm_* call for `sm` happens while this is held; it also guards the
	//! single-driver election transitions (`drive`, `opened`) and the per-reduce continuation election.
	mutex mu;
	//! Owning kernel scan SM (freed in the destructor).
	void *sm = nullptr;
	//! Single-driver election (NOT_STARTED -> DRIVING -> DONE | FAILED). The higher-level guard that makes
	//! the SM drive itself un-raceable. Atomic so a worker can read it under the source-state lock
	//! (lost-wakeup discipline) without taking `mu`; transitions happen under `mu`.
	atomic<SMDriveState> drive {SMDriveState::NOT_STARTED};
	//! Set once the SM has been opened (kdf_scan_open), under `mu`.
	bool opened = false;
	//! Error message captured when the drive fails (drive == FAILED).
	string error;

	//! Per-reduce hand-off channel (heap, ref-counted). Allocated when the first reduce is dispatched and
	//! co-owned by every outstanding reduce task. `reduce->state` is the continuation election.
	shared_ptr<ReduceChannel> reduce;
};

} // namespace duckdb
