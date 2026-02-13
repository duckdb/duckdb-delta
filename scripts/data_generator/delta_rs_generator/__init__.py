from deltalake import DeltaTable, write_deltalake
from pyspark.sql import SparkSession
from delta import *
from pyspark.sql.functions import *
import duckdb
import pandas as pd
import os
import shutil
import math
import glob

def generate_test_data_delta_rs_multi(base_path, path, init, tables, splits = 1):
    """
    generate_test_data_delta_rs generates some test data using delta-rs and duckdb

    :param path: the test data path (prefixed with BASE_PATH)
    :param init: a duckdb query initializes the duckdb tables that will be written
    :param tables: list of dicts containing the fields: name, query, (optionally) part_column
    :return: describe what it returns
    """
    generated_path = f"{base_path}/{path}"

    if (os.path.isdir(generated_path)):
        return
    try:
        os.makedirs(f"{generated_path}")

        # First we write a DuckDB file TODO: this should go in N appends as well?
        con = duckdb.connect(f"{generated_path}/duckdb.db", config={"allow_unsigned_extensions": "true"})

        con.sql(init)

        # Then we write the parquet files
        for table in tables:
            total_count = con.sql(f"select count(*) from ({table['query']})").fetchall()[0][0]
            # At least 1 tuple per file
            if total_count < splits:
                splits = total_count
            tuples_per_file = total_count // splits
            remainder = total_count % splits

            file_no = 0
            write_from = 0
            while file_no < splits:
                os.makedirs(f"{generated_path}/{table['name']}/parquet", exist_ok=True)
                # Write DuckDB's reference data
                write_to = write_from + tuples_per_file + (1 if file_no < remainder else 0)
                con.sql(f"COPY ({table['query']} where rowid >= {write_from} and rowid < {write_to}) to '{generated_path}/{table['name']}/parquet/data_{file_no}.parquet' (FORMAT parquet)")
                file_no += 1
                write_from = write_to

        for table in tables:
            con = duckdb.connect(f"{generated_path}/duckdb.db", config={"allow_unsigned_extensions": "true"})
            file_list = list(glob.glob(f"{generated_path}/{table['name']}/parquet/*.parquet"))
            file_list = sorted(file_list)
            for file in file_list:
                test_table_df = con.sql(f'from "{file}"').arrow()
                os.makedirs(f"{generated_path}/{table['name']}/delta_lake", exist_ok=True)
                write_deltalake(f"{generated_path}/{table['name']}/delta_lake", test_table_df, mode="append")
    except:
        if (os.path.isdir(generated_path)):
            shutil.rmtree(generated_path)
        raise

def generate_test_data_delta_rs(base_path, path, query, part_column=False, add_golden_table=True):
    """
    generate_test_data_delta_rs generates some test data using delta-rs and duckdb

    :param path: the test data path (prefixed with base_path)
    :param query: a duckdb query that produces a table called 'test_table'
    :param part_column: Optionally the name of the column to partition by
    :return: describe what it returns
    """


    generated_path = f"{base_path}/{path}"

    if (os.path.isdir(generated_path)):
        return

    try:
        con = duckdb.connect(config={"allow_unsigned_extensions": "true"})

        con.sql(query)

        # Write delta table data
        test_table_arrow = con.sql("FROM test_table;").arrow()

        if (part_column):
            write_deltalake(f"{generated_path}/delta_lake", test_table_arrow,  partition_by=[part_column])
        else:
            write_deltalake(f"{generated_path}/delta_lake", test_table_arrow)

        if add_golden_table:
            # Write DuckDB's reference data
            os.mkdir(f'{generated_path}/duckdb')
            if (part_column):
                con.sql(f"COPY test_table to '{generated_path}/duckdb' (FORMAT parquet, PARTITION_BY {part_column})")
            else:
                con.sql(f"COPY test_table to '{generated_path}/duckdb/data.parquet' (FORMAT parquet)")
    except:
        if (os.path.isdir(generated_path)):
            shutil.rmtree(generated_path)
        raise


__all__ = ["generate_test_data_delta_rs", "generate_test_data_delta_rs_multi"]
