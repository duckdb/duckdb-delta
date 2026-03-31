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
import json


def _get_spark_session():
    builder = SparkSession.builder.appName("MyApp") \
        .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension") \
        .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog") \
        .config("spark.driver.memory", "8g") \
        .config('spark.driver.host','127.0.0.1')
    return configure_spark_with_delta_pip(builder).getOrCreate()


def generate_test_data_pyspark(base_path, name, current_path,
                                input_path=None, base_query=None, queries=None,
                                delete_predicate=False, partition_column=None,
                                mapping_mode=None, domain_metadata_entries=None):
    """
    Generate test data as a Delta table using PySpark.

    Two modes of table creation:
      - input_path: create from a parquet file, also writes a parquet reference copy
      - base_query: create from a SQL query

    :param base_path: root path for generated test data
    :param name: spark table name (also used in path if current_path matches)
    :param current_path: relative path under base_path for output
    :param input_path: path to input parquet file (mutually exclusive with base_query)
    :param base_query: SQL SELECT to use as initial table data (mutually exclusive with input_path)
    :param queries: list of follow-up SQL statements to run after table creation
    :param delete_predicate: if set, enable deletion vectors and delete matching rows
    :param partition_column: column name to partition by
    :param mapping_mode: 'name' or 'id' for column mapping, None for default
    :param domain_metadata_entries: list of (domain, configuration) tuples to write as domain metadata
    """

    if input_path is None and base_query is None:
        raise ValueError("Either input_path or base_query must be provided")

    full_path = base_path + '/' + current_path
    if (os.path.isdir(full_path)):
        return

    try:
        spark = _get_spark_session()
        delta_table_path = full_path + '/delta_lake'
        os.makedirs(delta_table_path, exist_ok=True)

        if input_path is not None:
            ## Create table from parquet file
            parquet_reference_path = full_path + '/parquet'
            os.makedirs(parquet_reference_path, exist_ok=True)

            if partition_column:
                spark.sql(f"CREATE TABLE {name} USING delta PARTITIONED BY ({partition_column}) LOCATION '{delta_table_path}' AS SELECT * FROM parquet.`{input_path}`")
            else:
                spark.sql(f"CREATE TABLE {name} USING delta LOCATION '{delta_table_path}' AS SELECT * FROM parquet.`{input_path}`")

            if mapping_mode == 'name' or mapping_mode == 'id':
                spark.sql(f"ALTER TABLE {name} SET TBLPROPERTIES ('delta.minReaderVersion' = '3', 'delta.minWriterVersion' = '7', 'delta.columnMapping.mode' = '{mapping_mode}');")
            elif mapping_mode is None:
                spark.sql(f"ALTER TABLE {name} SET TBLPROPERTIES ('delta.minReaderVersion' = '3', 'delta.minWriterVersion' = '7');")
            else:
                raise ValueError(f"Unknown mapping mode: {mapping_mode}")

            if delete_predicate:
                spark.sql(f"ALTER TABLE {name} SET TBLPROPERTIES ('delta.enableDeletionVectors' = true);")
                deltaTable = DeltaTable.forPath(spark, delta_table_path)
                deltaTable.delete(delete_predicate)

            ## Write parquet reference
            df = spark.table(name)
            df.write.parquet(parquet_reference_path, mode='overwrite')

        else:
            ## Create table from SQL query
            if mapping_mode == 'name' or mapping_mode == 'id':
                spark.sql(
                    f"CREATE TABLE {name} USING delta TBLPROPERTIES ('delta.minReaderVersion' = '2', 'delta.minWriterVersion' = '5', 'delta.columnMapping.mode' = '{mapping_mode}', 'delta.enableTypeWidening' = 'true') LOCATION '{delta_table_path}' AS {base_query};")
            elif mapping_mode is None:
                spark.sql(f"CREATE TABLE {name} USING delta TBLPROPERTIES ('delta.minReaderVersion' = '2', 'delta.minWriterVersion' = '5', 'delta.enableTypeWidening' = 'true') LOCATION '{delta_table_path}' AS {base_query};")
            else:
                raise ValueError(f"Unknown mapping mode: {mapping_mode}")

        ## Run follow-up queries
        if queries:
            for query in queries:
                spark.sql(query)

        ## Write domain metadata by appending a commit to the delta log
        if domain_metadata_entries:
            log_dir = delta_table_path + '/_delta_log'
            existing = sorted(glob.glob(log_dir + '/*.json'))
            next_version = len(existing)
            commit_path = f"{log_dir}/{next_version:020d}.json"

            with open(commit_path, 'w') as f:
                f.write(json.dumps({"commitInfo": {"operation": "DOMAIN METADATA", "operationParameters": {}}}) + '\n')
                for domain, configuration in domain_metadata_entries:
                    f.write(json.dumps({"domainMetadata": {"domain": domain, "configuration": configuration, "removed": False}}) + '\n')

    except:
        if (os.path.isdir(full_path)):
            shutil.rmtree(full_path)
        raise


__all__ = ["generate_test_data_pyspark"]
