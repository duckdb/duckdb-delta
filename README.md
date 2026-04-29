# DuckDB Delta Extension

Read and write support for [Delta Lake](https://delta.io/) tables in DuckDB, built on [delta-kernel-rs](https://github.com/delta-incubator/delta-kernel-rs).

For full documentation, see [duckdb.org/docs/extensions/delta](https://duckdb.org/docs/extensions/delta).

## Quick Start

```sql
CREATE SECRET (
    TYPE s3,
    PROVIDER credential_chain
);
FROM delta_scan('s3://my-bucket/my-delta-table');
```

## Building

See the [Extension Template](https://github.com/duckdb/extension-template) for generic build instructions.

## Running Tests

Tests use DuckDB's sqllogictest framework. Test categories:

- `test/sql/dat/` — Delta Acceptance Tests (DAT)
- `test/sql/delta_kernel_rs/` — kernel-specific tests
- `test/sql/generated/` — generated data tests (requires `GENERATED_DATA_AVAILABLE=1`)
- `test/sql/golden_tests/` — golden table regression tests
- `test/sql/main/` — core functionality tests

```shell
# debug
make test_debug

# release
make test

# with generated data
make generate-data
GENERATED_DATA_AVAILABLE=1 make test
```

# Updating delta-kernel-rs / FFI version

Update the `GIT_TAG` in `./CMakeLists.txt` and re-run `make clean <debug|release>`. The FFI header is included directly from the cargo build.
