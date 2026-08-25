"""Check that Spark accepts a column-mapped table DuckDB has written to.

Spark creates and seeds the table, DuckDB appends a row, Spark reads it back. This is the only
evidence another engine resolves what we write, and the check to run after touching the
column-mapping write path.

Needs a python with pyspark and delta-spark; the `generate-data` make target pins versions that work:

    uv venv sparkenv && uv pip install --python sparkenv/bin/python delta-spark==4.0.0 pyspark==4.0.1
    ./sparkenv/bin/python scripts/verify_column_mapping_roundtrip.py [workdir] [name|id]

The table is created with `delta.feature.columnMapping = supported` so its protocol carries that one
writer feature and nothing else the kernel would refuse to write.

Exit status: 0 success, 1 Spark could not resolve the row, 2 the DuckDB write failed, 3 bad usage.
"""

import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/cm_roundtrip").resolve()
MODE = sys.argv[2] if len(sys.argv) > 2 else "name"
if MODE not in ("name", "id"):
    print(f"mode must be 'name' or 'id', got {MODE!r}", file=sys.stderr)
    sys.exit(3)

duckdb_bin = next((REPO / "build" / b / "duckdb" for b in ("release", "debug")
                   if (REPO / "build" / b / "duckdb").exists()), None)
if duckdb_bin is None:
    print("no duckdb binary under build/{release,debug} -- build the extension first", file=sys.stderr)
    sys.exit(3)

shutil.rmtree(WORK, ignore_errors=True)

from pyspark.sql import SparkSession  # noqa: E402  (import after the cheap checks above)

spark = (SparkSession.builder.appName("column-mapping-roundtrip")
         .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
         .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
         .config("spark.jars.packages", "io.delta:delta-spark_2.13:4.0.0")
         .config("spark.driver.host", "127.0.0.1")
         .master("local[1]").getOrCreate())
spark.sparkContext.setLogLevel("ERROR")

spark.sql(f"""CREATE TABLE delta.`{WORK}` (id INT, code STRING) USING DELTA
              TBLPROPERTIES ('delta.feature.columnMapping' = 'supported',
                             'delta.columnMapping.mode' = '{MODE}')""")
spark.sql(f"INSERT INTO delta.`{WORK}` VALUES (1, 'from-spark')")
print(f"spark:  created a {MODE}-mode table and seeded one row")

result = subprocess.run(
    [str(duckdb_bin), "-c",
     f"LOAD delta; ATTACH '{WORK}' AS t (TYPE delta); INSERT INTO t VALUES (4242, 'from-duckdb');"],
    capture_output=True, text=True)
if result.returncode != 0:
    print(f"duckdb: write FAILED\n{result.stdout}\n{result.stderr}")
    spark.stop()
    sys.exit(2)
print(f"duckdb: appended one row, {len(list(WORK.glob('duckdb-*.parquet')))} file(s) written")

spark.catalog.clearCache()
rows = {tuple(r) for r in spark.read.format("delta").load(str(WORK)).collect()}
print(f"spark:  read back {sorted(rows)}")

expected = (4242, "from-duckdb")
if expected in rows:
    print(f"PASS [{MODE} mode]: spark resolved the row duckdb wrote")
    status = 0
else:
    # In name mode an unresolved column comes back NULL; in id mode the file is refused outright.
    print(f"FAIL [{MODE} mode]: {expected} is missing -- spark could not resolve what duckdb wrote")
    status = 1

spark.stop()
sys.exit(status)
