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

BASE_PATH = os.path.dirname(os.path.realpath(__file__)) + "/../../data/generated"
TMP_PATH = '/tmp'

from delta_rs_generator import *
from pyspark_generator import *

################################################
### Simple tables for simple tests
################################################

## really simple
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"COPY (SELECT i FROM range(0,10) tbl(i)) TO '{TMP_PATH}/simple_table.parquet'")
generate_test_data_pyspark(BASE_PATH, 'simple_table', 'simple_table', f'{TMP_PATH}/simple_table.parquet')

## really simple partitioned
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"COPY (SELECT i, i%2 as part FROM range(0,10) tbl(i)) TO '{TMP_PATH}/simple_table_partitioned.parquet'")
generate_test_data_pyspark(BASE_PATH,'simple_table_partitioned', 'simple_table_partitioned', f'{TMP_PATH}/simple_table_partitioned.parquet', partition_column='part')

## really simple column mapped
## Table with simple evolution: adding a column
base_query = 'SELECT i, i%2 as part FROM range(0,9) tbl(i);'
queries = [
    'ALTER TABLE simple_table_column_mapped ADD COLUMN new_col BIGINT;',
    'INSERT INTO simple_table_column_mapped VALUES (9, 1, 1337);'
]
generate_test_data_pyspark_by_queries(BASE_PATH,'simple_table_column_mapped', 'simple_table_column_mapped', base_query, queries, 'name')

## really simple column mapped
## Table with simple evolution: adding a column
base_query = 'SELECT i, i%2 as part FROM range(0,9) tbl(i);'
queries = [
    'ALTER TABLE simple_table_column_mapped_by_id ADD COLUMN new_col BIGINT;',
    'INSERT INTO simple_table_column_mapped_by_id VALUES (9, 1, 1337);'
]
generate_test_data_pyspark_by_queries(BASE_PATH,'simple_table_column_mapped_by_id', 'simple_table_column_mapped_by_id', base_query, queries, 'name')

################################################
### TPC-H
################################################

## TPC-H SF0 PYSPARK
if (not os.path.isdir(BASE_PATH + '/tpch_sf0')):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.query(f"call dbgen(sf=0); EXPORT DATABASE '{TMP_PATH}/tpch_sf0_export' (FORMAT parquet)")
    for table in ["customer","lineitem","nation","orders","part","partsupp","region","supplier"]:
        generate_test_data_pyspark(BASE_PATH,f"tpch_sf0_{table}", f'tpch_sf0/{table}', f'{TMP_PATH}/tpch_sf0_export/{table}.parquet')
    con.query(f"attach '{BASE_PATH + '/tpch_sf0/duckdb.db'}' as duckdb_out")
    for table in ["customer","lineitem","nation","orders","part","partsupp","region","supplier"]:
        con.query(f"create table duckdb_out.{table} as from {table}")

## TPC-H SF0.01 PYSPARK
if (not os.path.isdir(BASE_PATH + '/tpch_sf0_01')):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.query(f"call dbgen(sf=0.01); EXPORT DATABASE '{TMP_PATH}/tpch_sf0_01_export' (FORMAT parquet)")
    for table in ["customer","lineitem","nation","orders","part","partsupp","region","supplier"]:
        generate_test_data_pyspark(BASE_PATH,f"tpch_sf0_01_{table}", f'tpch_sf0_01/{table}', f'{TMP_PATH}/tpch_sf0_01_export/{table}.parquet')
    con.query(f"attach '{BASE_PATH + '/tpch_sf0_01/duckdb.db'}' as duckdb_out")
    for table in ["customer","lineitem","nation","orders","part","partsupp","region","supplier"]:
        con.query(f"create table duckdb_out.{table} as from {table}")

## TPC-H SF1 PYSPARK
if (not os.path.isdir(BASE_PATH + '/tpch_sf1')):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.query(f"call dbgen(sf=1); EXPORT DATABASE '{TMP_PATH}/tpch_sf1_export' (FORMAT parquet)")
    for table in ["customer","lineitem","nation","orders","part","partsupp","region","supplier"]:
        generate_test_data_pyspark(BASE_PATH,f"tpch_sf1_{table}", f'tpch_sf1/{table}', f'{TMP_PATH}/tpch_sf1_export/{table}.parquet')
    con.query(f"attach '{BASE_PATH + '/tpch_sf1/duckdb.db'}' as duckdb_out")
    for table in ["customer","lineitem","nation","orders","part","partsupp","region","supplier"]:
        con.query(f"create table duckdb_out.{table} as from {table}")

################################################
### TPC-DS
################################################

## TPC-DS SF0 (schema only)
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"call dsdgen(sf=0); EXPORT DATABASE '{TMP_PATH}/tpcds_sf0_export' (FORMAT parquet)")
for table in ["call_center","catalog_page","catalog_returns","catalog_sales","customer","customer_demographics","customer_address","date_dim","household_demographics","inventory","income_band","item","promotion","reason","ship_mode","store","store_returns","store_sales","time_dim","warehouse","web_page","web_returns","web_sales","web_site"]:
    generate_test_data_pyspark(BASE_PATH,f"tpcds_sf0_{table}", f'tpcds_sf0/{table}', f'{TMP_PATH}/tpcds_sf0_export/{table}.parquet')

## TPC-DS SF0.01 full dataset
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"call dsdgen(sf=0.01); EXPORT DATABASE '{TMP_PATH}/tpcds_sf0_01_export' (FORMAT parquet)")
for table in ["call_center","catalog_page","catalog_returns","catalog_sales","customer","customer_demographics","customer_address","date_dim","household_demographics","inventory","income_band","item","promotion","reason","ship_mode","store","store_returns","store_sales","time_dim","warehouse","web_page","web_returns","web_sales","web_site"]:
    generate_test_data_pyspark(BASE_PATH,f"tpcds_sf0_01_{table}", f'tpcds_sf0_01/{table}', f'{TMP_PATH}/tpcds_sf0_01_export/{table}.parquet')

## TPC-DS SF1
if (not os.path.isdir(BASE_PATH + '/tpcds_sf1')):
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.query(f"call dsdgen(sf=1); EXPORT DATABASE '{TMP_PATH}/tpcds_sf1_export' (FORMAT parquet)")
    for table in ["call_center","catalog_page","catalog_returns","catalog_sales","customer","customer_demographics","customer_address","date_dim","household_demographics","inventory","income_band","item","promotion","reason","ship_mode","store","store_returns","store_sales","time_dim","warehouse","web_page","web_returns","web_sales","web_site"]:
        generate_test_data_pyspark(BASE_PATH,f"tpcds_sf1_{table}", f'tpcds_sf1/{table}', f'{TMP_PATH}/tpcds_sf1_export/{table}.parquet')
    con.query(f"attach '{BASE_PATH + '/tpcds_sf1/duckdb.db'}' as duckdb_out")
    for table in ["call_center","catalog_page","catalog_returns","catalog_sales","customer","customer_demographics","customer_address","date_dim","household_demographics","inventory","income_band","item","promotion","reason","ship_mode","store","store_returns","store_sales","time_dim","warehouse","web_page","web_returns","web_sales","web_site"]:
        con.query(f"create table duckdb_out.{table} as from {table}")

################################################
### Partitioned test data
################################################

### Simple partitioned table
query = "CREATE table test_table AS SELECT i, i%2 as part from range(0,10) tbl(i);"
generate_test_data_delta_rs(BASE_PATH,"simple_partitioned", query, "part")

### Partitioned table with + symbol
query = "CREATE table test_table AS SELECT i, (i%2)::VARCHAR || '+/' as part from range(0,10) tbl(i);"
generate_test_data_delta_rs(BASE_PATH,"simple_partitioned_with_url_encoding", query, "part")

### Simple partitioned table
query = "CREATE table test_table AS SELECT i, i%20 as part from range(0,10000) tbl(i);"
generate_test_data_delta_rs(BASE_PATH,"simple_partitioned_large", query, "part")

### Lineitem SF0.01 10 Partitions
query = "call dbgen(sf=0.01);"
query += "CREATE table test_table AS SELECT *, l_orderkey%10 as part from lineitem;"
generate_test_data_delta_rs(BASE_PATH,"lineitem_sf0_01_10part", query, "part")

## Partitioned table with all types we can file skip on
for type in ["bool", "int", "tinyint", "smallint", "bigint", "float", "double", "varchar"]:
    query = f"CREATE table test_table as select i::{type} as value1, (i)::{type} as value2, (i)::{type} as value3, i::{type} as part from range(0,5) tbl(i)"
    generate_test_data_delta_rs(BASE_PATH,f"test_file_skipping/{type}", query, "part")

## Partitioned table with date type
type = "date"
query = f"CREATE table test_table as select ('1994-01-0' || i::VARCHAR)::DATE as value1, ('1994-01-0' || i::VARCHAR)::DATE as part from range(1,6) tbl(i)"
generate_test_data_delta_rs(BASE_PATH,f"test_file_skipping/{type}", query, "part")

## Partitioned table with all types we can file skip on
for type in ["int"]:
    query = f"CREATE table test_table as select i::{type}+10 as value1, (i)::{type}+100 as value2, (i)::{type}+1000 as value3, i::{type} as part from range(0,5) tbl(i)"
    generate_test_data_delta_rs(BASE_PATH,f"test_file_skipping_2/{type}", query, "part")

################################################
### Testing specific data types
################################################

## Simple table with a blob as a value
query = "create table test_table as SELECT encode('ABCDE') as blob, encode('ABCDE') as blob_part, 'ABCDE' as string UNION ALL SELECT encode('😈') as blob, encode('😈') as blob_part, '😈' as string"
generate_test_data_delta_rs(BASE_PATH,"simple_blob_table", query, "blob_part", add_golden_table=False)

## Simple partitioned table with structs
query = "CREATE table test_table AS SELECT {'i':i, 'j':i+1} as value, i%2 as part from range(0,10) tbl(i);"
generate_test_data_delta_rs(BASE_PATH,"simple_partitioned_with_structs", query, "part")

################################################
### Deletion vectors
################################################

## Simple table with deletion vector
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"COPY (SELECT i as id, ('val' || i::VARCHAR) as value  FROM range(0,1000000) tbl(i))TO '{TMP_PATH}/simple_sf1_with_dv.parquet'")
generate_test_data_pyspark(BASE_PATH,'simple_sf1_with_dv', 'simple_sf1_with_dv', f'{TMP_PATH}/simple_sf1_with_dv.parquet', "id % 1000 = 0")

## Lineitem SF0.01 with deletion vector
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"call dbgen(sf=0.01); COPY (from lineitem) TO '{TMP_PATH}/modified_lineitem_sf0_01.parquet'")
generate_test_data_pyspark(BASE_PATH,'lineitem_sf0_01_with_dv', 'lineitem_sf0_01_with_dv', f'{TMP_PATH}/modified_lineitem_sf0_01.parquet', "l_shipdate = '1994-01-01'")

## Lineitem SF1 with deletion vector
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.query(f"call dbgen(sf=1); COPY (from lineitem) TO '{TMP_PATH}/modified_lineitem_sf1.parquet'")
generate_test_data_pyspark(BASE_PATH,'lineitem_sf1_with_dv', 'lineitem_sf1_with_dv', f'{TMP_PATH}/modified_lineitem_sf1.parquet', "l_shipdate = '1994-01-01'")

################################################
### Schema evolution
################################################

## Table with simple evolution: adding a column
base_query = 'select CAST(1 as INT) as a;'
queries = [
    'ALTER TABLE evolution_simple ADD COLUMN b BIGINT;',
    'INSERT INTO evolution_simple VALUES (2, 2);'
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_simple', 'evolution_simple', base_query, queries)

## Table with simple evolution with id mode: adding a column
base_query = 'select CAST(1 as INT) as a;'
queries = [
    'ALTER TABLE evolution_simple_id_mode ADD COLUMN b BIGINT;',
    'INSERT INTO evolution_simple_id_mode VALUES (2, 2);'
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_simple_id_mode', 'evolution_simple_id_mode', base_query, queries, 'name')

## Table with simple evolution with id mode: adding a column
base_query = 'select CAST(1 as INT) as a;'
queries = [
    'ALTER TABLE evolution_simple_name_mode ADD COLUMN b BIGINT;',
    'INSERT INTO evolution_simple_name_mode VALUES (2, 2);'
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_simple_name_mode', 'evolution_simple_name_mode', base_query, queries, 'name')

## Table that drops and re-adds a column with the same name for max confusion
base_query = "select 'value1' as a, 'value2' as b;"
queries = [
    "ALTER TABLE evolution_column_change DROP COLUMN b;",
    "INSERT INTO evolution_column_change VALUES ('value3');",
    "ALTER TABLE evolution_column_change ADD COLUMN b BIGINT;",
    "INSERT INTO evolution_column_change VALUES ('value4', 5);",
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_column_change', 'evolution_column_change', base_query, queries, 'name')

## Table that drops and re-adds a column with the same name for max confusion
base_query = "select 'value1' as a, 'value2' as b;"
queries = [
    "ALTER TABLE evolution_column_change_id_mode DROP COLUMN b;",
    "INSERT INTO evolution_column_change_id_mode VALUES ('value3');",
    "ALTER TABLE evolution_column_change_id_mode ADD COLUMN b BIGINT;",
    "INSERT INTO evolution_column_change_id_mode VALUES ('value4', 5);",
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_column_change_id_mode', 'evolution_column_change_id_mode', base_query, queries, 'id')

## CREATE table that has all type widenings from the spec
base_query = "select CAST(42 AS BYTE) as integer, CAST(42.42 AS FLOAT) as float, CAST(42 AS INT) as int_to_double, CAST('2042-01-01' AS DATE) as date, CAST('42.42' as DECIMAL(4,2)) as decimal, CAST(42 AS INT) as int_to_decimal, CAST(42 AS BIGINT) as long_to_decimal"
queries = [
    "ALTER TABLE evolution_type_widening ALTER COLUMN integer TYPE SMALLINT;",
    # TODO: add these once pyspark supports it
    # "ALTER TABLE evolution_type_widening ALTER COLUMN float TYPE DOUBLE;",
    # "ALTER TABLE evolution_type_widening ALTER COLUMN int_to_double TYPE DOUBLE;",
    # "ALTER TABLE evolution_type_widening ALTER COLUMN date TYPE TIMESTAMP_NTZ;",
    # "ALTER TABLE evolution_type_widening ALTER COLUMN decimal TYPE DECIMAL(5,2);",
    # "ALTER TABLE evolution_type_widening ALTER COLUMN int_to_decimal TYPE DECIMAL(5,2);",
    # "ALTER TABLE evolution_type_widening ALTER COLUMN long_to_decimal TYPE DECIMAL(5,2);",
    "INSERT INTO evolution_type_widening VALUES (42, 42.42, 42, '2042-01-01', 42.42, 42, 42);",
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_type_widening', 'evolution_type_widening', base_query, queries, 'name')

## CREATE table that has struct widening
base_query = "select named_struct('struct_field_a', 'value1', 'struct_field_b', 'value2') as top_level_column;"
queries = [
    "ALTER TABLE evolution_struct_field_modification ADD COLUMNS (top_level_column.struct_field_c STRING AFTER struct_field_b)",
    "INSERT INTO evolution_struct_field_modification VALUES (named_struct('struct_field_a', 'value3', 'struct_field_b', 'value4', 'struct_field_c', 'value5'));",
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_struct_field_modification', 'evolution_struct_field_modification', base_query, queries, 'name')

## CREATE table that has nested struct widening
base_query = "select named_struct('top_level_struct', named_struct('struct_field_a', 'value1', 'struct_field_b', 'value2')) as top_level_column;"
queries = [
    "ALTER TABLE evolution_struct_field_modification_nested ADD COLUMNS (top_level_column.top_level_struct.struct_field_c STRING AFTER struct_field_b)",
    "INSERT INTO evolution_struct_field_modification_nested VALUES (named_struct('top_level_struct', named_struct('struct_field_a', 'value3', 'struct_field_b', 'value4', 'struct_field_c', 'value5')));",
]
generate_test_data_pyspark_by_queries(BASE_PATH,'evolution_struct_field_modification_nested', 'evolution_struct_field_modification_nested', base_query, queries, 'name')
