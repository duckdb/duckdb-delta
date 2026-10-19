"""Spark-written column-mapped Delta tables.

The tables are committed under data/inlined/column_mapping; CI does not run this. To regenerate, run:
python scripts/data_generator/generate_column_mapping_data.py [BASE_PATH]
Needs pyspark >= 4.2 + delta-spark >= 4.4. In a container set JAVA_TOOL_OPTIONS=-XX:-UseContainerSupport.
Each table lands in <BASE_PATH>/column_mapping/spark_<name>_<mode>_mode.
"""

import os
import shutil
import sys

from delta import configure_spark_with_delta_pip
from pyspark.sql import SparkSession

DEFAULT_BASE_PATH = os.path.dirname(os.path.realpath(__file__)) + "/../../data/inlined"

# name -> the statements that build it; {t} is the table
TABLES = {
    # The partition column sits between two data columns, so the reader has to place it after a named column
    "partitioned": [
        "CREATE TABLE {t} (id INT, p STRING, v INT) USING delta PARTITIONED BY (p) TBLPROPERTIES ({props})",
        "INSERT INTO {t} VALUES (1, 'a', 10), (2, 'a', 20), (3, 'b', 30), (4, NULL, 40)",
    ],
    # Rows on both sides of the rename; the files keep the physical name throughout
    "renamed": [
        "CREATE TABLE {t} (id INT, s STRING) USING delta TBLPROPERTIES ({props})",
        "INSERT INTO {t} VALUES (1, 'before')",
        "ALTER TABLE {t} RENAME COLUMN s TO who",
        "INSERT INTO {t} VALUES (2, 'after')",
    ],
}

MODES = ["name", "id"]


def spark_session():
    builder = (
        SparkSession.builder.appName("column_mapping")
        .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
        .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
        .config("spark.driver.host", "127.0.0.1")
        .config("spark.ui.enabled", "false")
        .config("spark.sql.shuffle.partitions", "1")
        .config("spark.default.parallelism", "1")
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
    base = os.path.join(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BASE_PATH, "column_mapping")
    spark = spark_session()
    for name, statements in TABLES.items():
        for mode in MODES:
            path = os.path.join(base, f"spark_{name}_{mode}_mode")
            shutil.rmtree(path, ignore_errors=True)
            table = f"delta.`{path}`"
            props = f"'delta.columnMapping.mode' = '{mode}'"
            for statement in statements:
                spark.sql(statement.format(t=table, props=props))
            remove_hadoop_checksums(path)
            rows = spark.read.format("delta").load(path).orderBy("id").collect()
            print(os.path.basename(path), [row.asDict() for row in rows])


if __name__ == "__main__":
    main()
