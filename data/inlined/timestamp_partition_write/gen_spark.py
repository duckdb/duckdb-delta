"""Spark fixtures for test/sql/main/writing/timestamp_partition_write.test: gen_spark.py <out dir>.
pyspark 4.2.0 + delta-spark 4.4.1, session TimeZone America/Los_Angeles (Spark names timestamp partition
directories in that zone; the log holds the UTC instant as ISO 8601 with Z)."""
import glob, os, shutil, sys
from delta import configure_spark_with_delta_pip
from pyspark.sql import SparkSession
out = sys.argv[1]
b = (SparkSession.builder.config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
     .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
     .config("spark.driver.host", "127.0.0.1").config("spark.ui.enabled", "false")
     .config("spark.sql.session.timeZone", "America/Los_Angeles"))
s = configure_spark_with_delta_pip(b).getOrCreate()
s.sparkContext.setLogLevel("ERROR")
tables = {
    "spark_timestamp": ("(id INT, p TIMESTAMP) USING delta PARTITIONED BY (p)",
                        "(1, TIMESTAMP '2024-01-01 00:00:00+00:00')"),
    "spark_timestamp_mapped": ("(id INT, p TIMESTAMP) USING delta PARTITIONED BY (p) "
                               "TBLPROPERTIES ('delta.columnMapping.mode' = 'name')",
                               "(1, TIMESTAMP '2024-01-01 00:00:00+00:00')"),
    "spark_binary": ("(id INT, b BINARY) USING delta PARTITIONED BY (b)", "(1, X'616263')"),
}
for name, (ddl, row) in tables.items():
    path = os.path.join(out, name)
    shutil.rmtree(path, ignore_errors=True)
    s.sql(f"CREATE TABLE delta.`{path}` {ddl}")
    s.sql(f"INSERT INTO delta.`{path}` VALUES {row}")
    for junk in glob.glob(f"{path}/**/.*.crc", recursive=True) + glob.glob(f"{path}/_delta_log/*.crc"):
        os.remove(junk)
    shutil.rmtree(f"{path}/_delta_log/_staged_commits", ignore_errors=True)
