# Prototype: DuckDB-driven Delta plan-based scan

DuckDB drives the delta-kernel-rs **scan state machines** and executes the lowered
declarative plan as its own operators — log replay, checkpoint/sidecar reconciliation,
dedup, tombstones, deletion vectors, column mapping, and **data skipping** all become
visible DuckDB operators.

## Two repos (matching prototype branches)
- **delta-kernel-rs** `prototype/duckdb-plan-based-scan` — the `duckdb` FFI feature:
  steppable scan/snapshot state machines (`ffi/src/duckdb/sm.rs`), plan→SQL lowering
  (`ffi/src/duckdb/plan_to_sql.rs`), and the data-skipping filter in the scan
  reconciliation (`kernel/src/plans/state_machines/scan/reconciliation.rs`).
- **duckdb-delta** `prototype/duckdb-plan-based-scan` — the extension: the faithful
  `delta_load` operator, `delta_scan` / `delta_scan_metadata`, execution-time SM drive
  (sync + async), and dynamic predicate pushdown.

## Build
```bash
# kernel FFI
cd delta-kernel-rs
cargo build --release -p delta_kernel_ffi \
  --features "default-engine-rustls,arrow,test-ffi,delta-kernel-unity-catalog,tracing,duckdb"

# extension (CMake ExternalProject points at the local kernel)
cd duckdb-delta
CMAKE_BUILD_PARALLEL_LEVEL=24 make release
```
Run: `build/release/duckdb -unsigned`. The plan-based path is gated:
`DELTA_KERNEL_PLAN_SM=1` (execution-time SM drive + dynamic pushdown),
`DELTA_KERNEL_PLAN_SM_ASYNC=1` (per-request async, non-blocking metadata).

## Try it
```sql
-- metadata scan: the surviving file list (drives the metadata-only SM)
SELECT path, size FROM delta_scan_metadata('<table>/delta');

-- data scan
SELECT * FROM delta_scan('<table>/delta');

-- the reconciliation as visible DuckDB operators
EXPLAIN SELECT * FROM delta_scan('<table>/delta');

-- data skipping: the stats FILTER node is visible, driven by the WHERE (no env var)
EXPLAIN SELECT * FROM delta_scan('<table>/delta') WHERE value > 1500;
EXPLAIN ANALYZE SELECT * FROM delta_scan('<table>/delta') WHERE value > 1500;  -- Scanning Files: N/M
```
Good demo tables (kernel `acceptance/workloads/<name>/delta`): `DV-001` (deletion
vectors), `cp_v2_basic` (V2 checkpoint + sidecars), `cm_rename_partition_col` (column
mapping), `var_001_basic` (variant type).

## Status (honest)
- Acceptance corpus on the SM path: **1643/1656** (sync and async identical).
- Native C++ acceptance harness: `make acceptance_harness_release` then
  `DELTA_KERNEL_PLAN_SM=1 build/release/extension/delta/acceptance_harness`
  (`--concurrency N` exercises the async path intra-process).
- **Gated, not default** — plain `delta_scan` (no flag) still uses the legacy path.
- Remaining 13 corpus failures are pre-existing kernel-SQL gaps (variant shredding,
  timestamp time-travel, some type-widening / column-mapping edges), not regressions.
- Async UAF-safety validated by abort-stress, not ASan.

---

## Full reproduction

This prototype spans three forks, all on branch `prototype/duckdb-plan-based-scan`:

| repo | fork | pinned rev |
|---|---|---|
| DuckDB (core) | `OussamaSaoudi/duckdb` | `f33b442698` |
| delta-kernel-rs | `OussamaSaoudi/delta-kernel-rs` | `dadf1c12c1` |
| duckdb-delta (this) | `OussamaSaoudi/duckdb-delta` | `e658d006f4` |

The duckdb-delta CMake fetches the matching `delta-kernel-rs` branch, and the vendored
`duckdb` submodule points at the fork above (it carries the core changes this needs:
`BY_FIELD_ID_OR_NAME` column mapping, a table-arg child on `LOGICAL_EXTENSION_OPERATOR`,
and the streaming sink+source).

### Build
```bash
git clone --branch prototype/duckdb-plan-based-scan --recurse-submodules \
  https://github.com/OussamaSaoudi/duckdb-delta
cd duckdb-delta
# the CMake ExternalProject clones + builds the delta-kernel-rs branch itself
CMAKE_BUILD_PARALLEL_LEVEL=24 make release
```
Produces `build/release/duckdb` (CLI) and `build/release/extension/delta/delta.duckdb_extension`.

### Run — no env vars, plan-based scan is the default
```bash
DDB=build/release/duckdb          # run with -unsigned
T=<path-to-a-delta-table>         # e.g. a delta-kernel-rs acceptance workload: <wl>/delta

# scan the data
$DDB -unsigned -c "SELECT * FROM delta_scan('$T');"

# the metadata phase (surviving file list) as its own function
$DDB -unsigned -c "SELECT * FROM delta_scan_metadata('$T');"

# the reconciliation as visible DuckDB operators
$DDB -unsigned -c "EXPLAIN SELECT * FROM delta_scan('$T');"

# data skipping is dynamic, from the query WHERE (kernel stats prune files before the read)
$DDB -unsigned -c "EXPLAIN ANALYZE SELECT * FROM delta_scan('$T') WHERE <col> > <lit>;"
#   -> the DYNAMIC_SCAN node shows 'File Filters' + 'Scanning Files: N/M'
```

### Verify (acceptance corpus)
```bash
# in-process C++ harness: runs the delta-kernel-rs acceptance/workloads read specs through
# delta_scan and compares against the golden parquet. --concurrency N runs them in parallel.
make acceptance_harness_release
build/release/extension/delta/acceptance_harness            # -> pass=1643/1656
build/release/extension/delta/acceptance_harness --concurrency 8
```
The 13 non-passing specs are pre-existing kernel-SQL feature gaps (variant shredding,
timestamp time-travel, some type-widening / column-mapping edges), not regressions.
