"""Spark 4.2 / delta-spark 4.4.1: the Delta interval tables in data/inlined/interval/spark_interval*.

The tables are committed; CI does not run this. To regenerate:
  python scripts/data_generator/generate_interval_data.py data/inlined/interval
Needs pyspark >= 4.2 + delta-spark >= 4.4. In a container set JAVA_TOOL_OPTIONS=-XX:-UseContainerSupport.

The Delta protocol defines no interval type, and Delta-Spark refuses interval columns at CREATE unless
spark.databricks.delta.schema.typeCheck.enabled is false; with it off, Spark writes them as INT32 months / INT64 micros
and the log names them `interval year to month` / `interval day to second` (protocol 1/2, no table feature). Values
cover both kinds, Spark's sub-kinds, negatives, NULLs, the range ends, and intervals nested in structs, lists and maps,
next to an INT96 timestamp in one struct.
"""
import os, shutil, sys
from delta import configure_spark_with_delta_pip
from pyspark.sql import SparkSession

OUT = sys.argv[1]
b = (SparkSession.builder.appName("interval-fixtures")
     .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
     .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
     .config("spark.driver.host", "127.0.0.1").config("spark.ui.enabled", "false")
     .config("spark.sql.session.timeZone", "UTC").config("spark.sql.shuffle.partitions", "1")
     .config("spark.databricks.delta.schema.typeCheck.enabled", "false"))
spark = configure_spark_with_delta_pip(b).getOrCreate()
spark.sparkContext.setLogLevel("ERROR")
sql = spark.sql


def table(name, ddl, inserts, extra=""):
    path = os.path.join(OUT, name)
    shutil.rmtree(path, ignore_errors=True)
    sql(f"CREATE TABLE delta.`{path}` ({ddl}) USING delta {extra}")
    sql(f"INSERT INTO delta.`{path}` " + " UNION ALL ".join(inserts))
    for r in sql(f"SELECT CAST(struct(*) AS STRING) FROM delta.`{path}` ORDER BY id").collect():
        print(name, r[0])


table("spark_interval",
      "id INT, ym INTERVAL YEAR TO MONTH, dt INTERVAL DAY TO SECOND, y INTERVAL YEAR, h INTERVAL HOUR, "
      "s STRUCT<n: INTERVAL YEAR TO MONTH, gap: INTERVAL DAY TO SECOND>, "
      "l ARRAY<INTERVAL YEAR TO MONTH>, m MAP<STRING, INTERVAL DAY TO SECOND>",
      ["SELECT 1, INTERVAL '1-2' YEAR TO MONTH, INTERVAL '3 04:05:06.789123' DAY TO SECOND, INTERVAL '2' YEAR, "
       "INTERVAL '25' HOUR, named_struct('n', INTERVAL '1' MONTH, 'gap', INTERVAL '1' DAY), "
       "array(INTERVAL '1' MONTH, INTERVAL '-0-2' YEAR TO MONTH, NULL), map('a', INTERVAL '36' HOUR)",
       "SELECT 2, INTERVAL '-0-1' YEAR TO MONTH, INTERVAL '-0 00:00:00.000001' DAY TO SECOND, INTERVAL '-1' YEAR, "
       "INTERVAL '-1' HOUR, named_struct('n', INTERVAL '-1-0' YEAR TO MONTH, 'gap', NULL), array(), "
       "map('b', INTERVAL '-1 00:00:00' DAY TO SECOND)",
       "SELECT 3, NULL, NULL, NULL, NULL, NULL, NULL, NULL",
       "SELECT 4, INTERVAL '0-0' YEAR TO MONTH, INTERVAL '1' DAY, INTERVAL '0' YEAR, INTERVAL '24' HOUR, "
       "named_struct('n', INTERVAL '0' MONTH, 'gap', INTERVAL '24' HOUR), array(INTERVAL '12' MONTH), map()",
       "SELECT 5, INTERVAL '178956970-7' YEAR TO MONTH, INTERVAL '106751991 04:00:54.775807' DAY TO SECOND, "
       "NULL, NULL, NULL, NULL, NULL",
       "SELECT 6, INTERVAL '-178956970-8' YEAR TO MONTH, INTERVAL '-106751991 04:00:54.775808' DAY TO SECOND, "
       "NULL, NULL, NULL, NULL, NULL"])

# Deeper shapes, and a struct mixing Spark's INT96 timestamp with an interval (both need converting on read)
table("spark_interval_deep",
      "id INT, mk MAP<INTERVAL YEAR TO MONTH, STRING>, ll ARRAY<ARRAY<INTERVAL DAY TO SECOND>>, "
      "ls ARRAY<STRUCT<gap: INTERVAL DAY TO SECOND>>, st STRUCT<ts: TIMESTAMP, ym: INTERVAL YEAR TO MONTH>",
      ["SELECT 1, map(INTERVAL '1-2' YEAR TO MONTH, 'a'), array(array(INTERVAL '1' DAY, NULL), array()), "
       "array(named_struct('gap', INTERVAL '-1' HOUR)), "
       "named_struct('ts', TIMESTAMP'2024-01-01 00:00:00', 'ym', INTERVAL '1' MONTH)",
       "SELECT 2, NULL, NULL, NULL, named_struct('ts', NULL, 'ym', INTERVAL '-2' MONTH)"])

table("spark_interval_part_ym", "id INT, ym INTERVAL YEAR TO MONTH",
      ["VALUES (1, INTERVAL '1-2' YEAR TO MONTH), (2, INTERVAL '-3' MONTH), (3, NULL), (4, INTERVAL '1-2' YEAR TO MONTH)"],
      "PARTITIONED BY (ym)")
table("spark_interval_part_dt", "id INT, dt INTERVAL DAY TO SECOND",
      ["VALUES (1, INTERVAL '1 02:03:04.5' DAY TO SECOND), (2, INTERVAL '-0 00:00:00.000001' DAY TO SECOND), (3, NULL)"],
      "PARTITIONED BY (dt)")
table("spark_interval_colmap",
      "id INT, `y m` INTERVAL YEAR TO MONTH, s STRUCT<`g ap`: INTERVAL DAY TO SECOND>",
      ["SELECT 1, INTERVAL '1-2' YEAR TO MONTH, named_struct('g ap', INTERVAL '1' DAY)"],
      "TBLPROPERTIES ('delta.columnMapping.mode' = 'name')")

# Keep the committed tables small: Spark's hidden .crc sidecars and the empty catalog-commit dir carry nothing
for root, dirs, files in os.walk(OUT):
    for f in files:
        if f.startswith(".") and f.endswith(".crc"):
            os.remove(os.path.join(root, f))
    if os.path.basename(root) == "_staged_commits" and not os.listdir(root):
        os.rmdir(root)
