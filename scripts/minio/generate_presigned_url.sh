#!/usr/bin/env bash
#Note: DONT run as root

DUCKDB_PATH=duckdb
if command -v duckdb; then
  DUCKDB_PATH=duckdb
elif test -f build/release/duckdb; then
  DUCKDB_PATH=build/release/duckdb
elif test -f build/reldebug/duckdb; then
  DUCKDB_PATH=build/reldebug/duckdb
elif test -f build/debug/duckdb; then
  DUCKDB_PATH=build/debug/duckdb
fi

rm -rf test/test_data
mkdir -p test/test_data

generate_large_parquet_query=$(cat <<EOF

CALL DBGEN(sf=1);
COPY lineitem TO 'test/test_data/presigned-url-lineitem.parquet' (FORMAT 'parquet');

EOF
)
$DUCKDB_PATH -c "$generate_large_parquet_query"

# Generate Storage Version
# Attach with STORAGE_VERSION 'latest' so the fixture is written in a storage
# version that supports the current test_all_types() (which now includes an empty
# STRUCT column, unsupported prior to storage v2.0.0). Piping the script into a
# default v1.0.0 database aborts the CREATE TABLE all_types transaction, which
# drops integral_values/all_types/test3 and breaks attach_httpfs/attach_s3.
{ echo "ATTACH 'test/test_data/attach.db' AS db (STORAGE_VERSION 'latest'); USE db;"; \
  cat duckdb/test/sql/storage_version/generate_storage_version.sql; } | $DUCKDB_PATH :memory:
$DUCKDB_PATH  test/test_data/lineitem_sf1.db -c "CALL dbgen(sf=1)"
