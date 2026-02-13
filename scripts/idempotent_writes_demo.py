from decimal import Decimal
from pathlib import Path
import duckdb
import os
import shutil
import sys

PARQUET_OUTPUT_DIR = "/tmp/idempotency_demo_parquet"
DELTA_OUTPUT_DIR = "/tmp/idempotency_demo_delta"
APP_ID = "demo_app_id"
TABLE_NAME = "delta_table"


def get_newest_extension(builds=("release", "debug")):
    paths = map(
        lambda x: Path(f"build/{x}/extension/delta/delta.duckdb_extension"),
        list(builds),
    )
    exts = sorted(
        filter(Path.exists, paths),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not exts:
        print("Found no loadable delta.duckdb_extension; `make`?", file=sys.stderr)
        sys.exit(1)
    print(f"- Using extension={exts[0]}")
    return str(exts[0])


# Generates TPC-H lineitem table as multiple parquet files, creating a batch for each portion of data
def generate_test_data(scale_factor, num_batches, path):
    for batch_num in range(1, num_batches+1):
        con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
        con.execute(f"call dbgen(sf={scale_factor}, children={num_batches}, step={batch_num-1})")
        os.makedirs(f"{path}/{batch_num}", exist_ok=True)
        con.execute(f"COPY lineitem TO '{path}/{batch_num}/lineitem.parquet'")

def cleanup_data():
    dirpath = Path(DELTA_OUTPUT_DIR)
    if dirpath.exists() and dirpath.is_dir():
        shutil.rmtree(dirpath)

    dirpath = Path(PARQUET_OUTPUT_DIR)
    if dirpath.exists() and dirpath.is_dir():
        shutil.rmtree(dirpath)

def setup_duckdb_connection():
    ext = get_newest_extension()
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.query(f"LOAD '{ext}'")
    con.execute(f"ATTACH '{DELTA_OUTPUT_DIR}' AS {TABLE_NAME} (TYPE delta)")
    return con

def write_batch(con, batch_to_write, set_new_version = None, current_version = None):
    con.execute(f"BEGIN TRANSACTION")

    if current_version is None:
        current_version = 'NULL'

    # Initiate the compare-and-swap operation that will be performed on COMMIT: when committing, DuckDB will check
    # that the version of `APP_ID` is still equal to `current_version` and change it to current_version + 1
    if set_new_version is not None:
        con.execute(f"CALL delta_set_transaction_version('{TABLE_NAME}', '{APP_ID}', {set_new_version}::UBIGINT, {current_version}::UBIGINT);")

    # Write the batch to the delta table
    con.execute(f"INSERT INTO {TABLE_NAME} FROM '{PARQUET_OUTPUT_DIR}/{batch_to_write}/lineitem.parquet'")

    # Commit!
    con.execute(f"COMMIT")

def process_batches(input_path, max_batch = 0):
    print(f"Processing batches (max batch: {max_batch})")

    con = setup_duckdb_connection()

    while True:
        current_version = con.query(f"SELECT version FROM delta_get_transaction_version('{TABLE_NAME}', '{APP_ID}');").fetchall()[0][0]

        if current_version is not None and current_version >= max_batch:
            break

        next_batch = 1 if current_version is None else current_version + 1

        print(f"- Current version: {current_version}, Processing batch {next_batch} ({input_path}/{next_batch}/lineitem.parquet)")
        write_batch(con, next_batch, next_batch, current_version)

    print()

# Validates the output by checking the result of query 6 from TPC-H on the lineitem table
def validate_output(delta_dir, throw = True):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"CREATE VIEW lineitem AS FROM delta_scan('{delta_dir}');")
    result = con.sql("pragma tpch(6)").fetchall()[0][0];
    expected = Decimal('1193053.2253')
    result_type = type(result)
    expected_type = type(expected)

    if result != expected:
        if throw:
            raise Exception(f"Incorrect result: '{result}' != '{expected}' (types: '{result_type}' and '{expected_type}')")
        return False

    return True

def main():
    shutil.copytree('data/generated/tpch_sf0/lineitem/delta_lake', DELTA_OUTPUT_DIR)

    # Generate Lineitem TPCH SF-0.01 split in 10 batches
    generate_test_data(0.01, 10, PARQUET_OUTPUT_DIR)

    # Will write the first batch
    process_batches(PARQUET_OUTPUT_DIR, max_batch=1)
    process_batches(PARQUET_OUTPUT_DIR, max_batch=1)
    process_batches(PARQUET_OUTPUT_DIR, max_batch=5)
    process_batches(PARQUET_OUTPUT_DIR, max_batch=9)

    # print(f"Manually writing batch number 10\n")
    # con = setup_duckdb_connection()
    # write_batch(con, 10, 10, 9)

    process_batches(PARQUET_OUTPUT_DIR, max_batch=10)

    # Validate output
    validate_output(DELTA_OUTPUT_DIR)

    print("success!")

if __name__ == "__main__":
    cleanup_data()

    # Run main
    main()
