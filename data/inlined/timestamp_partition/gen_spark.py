"""Spark fixture for test/sql/inlined/timestamp_partition_timezone.test: gen_spark.py <out>.
Session TZ America/Los_Angeles: Spark names the directory in that zone, the log holds the UTC instant."""
import shutil, sys
from delta import configure_spark_with_delta_pip
from pyspark.sql import SparkSession
out = sys.argv[1]
shutil.rmtree(out, ignore_errors=True)
b = (SparkSession.builder.config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
     .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
     .config("spark.driver.host", "127.0.0.1").config("spark.ui.enabled", "false")
     .config("spark.sql.session.timeZone", "America/Los_Angeles"))
s = configure_spark_with_delta_pip(b).getOrCreate()
s.sparkContext.setLogLevel("ERROR")
s.sql(f"CREATE TABLE delta.`{out}` (id INT, p TIMESTAMP) USING delta PARTITIONED BY (p)")
s.sql(f"""INSERT INTO delta.`{out}` VALUES
  (1, TIMESTAMP '2023-06-01 00:00:00+00:00'), (2, TIMESTAMP '2023-12-31 22:00:00+00:00'),
  (3, TIMESTAMP '2024-01-01 00:00:00+00:00'), (4, TIMESTAMP '2024-01-01 03:00:00+00:00'), (5, NULL)""")
