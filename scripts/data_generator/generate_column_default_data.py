"""Spark-written Delta tables for the column-default (allowColumnDefaults) write refusal.

The tables are committed under data/inlined/column_defaults; CI does not run this. To regenerate, run:
python scripts/data_generator/generate_column_default_data.py [BASE_PATH]
Needs pyspark >= 4.2 + delta-spark >= 4.4. In a container set JAVA_TOOL_OPTIONS=-XX:-UseContainerSupport.
Each table lands in <BASE_PATH>/column_defaults/<name>/delta_lake.
"""

import os
import shutil
import sys

from delta import configure_spark_with_delta_pip
from pyspark.sql import SparkSession

DEFAULT_BASE_PATH = os.path.dirname(os.path.realpath(__file__)) + "/../../data/inlined"

# name -> (CREATE TABLE columns and properties, the INSERT that follows it)
TABLES = {
    "with_default": (
        "(id INT, x INT DEFAULT 7) USING delta TBLPROPERTIES ('delta.feature.allowColumnDefaults' = 'supported')",
        "(id) VALUES (1)",
    ),
    "feature_only": (
        "(id INT, x INT) USING delta TBLPROPERTIES ('delta.feature.allowColumnDefaults' = 'supported')",
        "VALUES (1, 1)",
    ),
    # writer version 7 with a feature list, so the check reads a list that lacks allowColumnDefaults
    "without_feature": (
        "(id INT, x INT) USING delta TBLPROPERTIES ('delta.feature.domainMetadata' = 'supported')",
        "VALUES (1, 1)",
    ),
}


def spark_session():
    builder = (
        SparkSession.builder.appName("column_defaults")
        .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
        .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
        .config("spark.driver.host", "127.0.0.1")
        .config("spark.ui.enabled", "false")
        .config("spark.sql.shuffle.partitions", "1")
    )
    spark = configure_spark_with_delta_pip(builder).getOrCreate()
    spark.sparkContext.setLogLevel("ERROR")
    return spark


def remove_hadoop_checksums(path):
    for root, _, files in os.walk(path):
        for name in files:
            if name.startswith(".") and name.endswith(".crc"):
                os.remove(os.path.join(root, name))


def main():
    base = os.path.join(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BASE_PATH, "column_defaults")
    shutil.rmtree(base, ignore_errors=True)
    spark = spark_session()
    for name, (create, insert) in TABLES.items():
        path = os.path.join(base, name, "delta_lake")
        spark.sql(f"CREATE TABLE delta.`{path}` {create}")
        spark.sql(f"INSERT INTO delta.`{path}` {insert}")
        remove_hadoop_checksums(path)
        print(name, [row.asDict() for row in spark.read.format("delta").load(path).collect()])


if __name__ == "__main__":
    main()
