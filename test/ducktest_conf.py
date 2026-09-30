from ducktest5.engines.duckdb import duckdb_engine
from ducktest5.engines.spark import delta_spark

# The only DuckDB engine is the default one, so the .test files run on it too. Tests write under the
# run's default storage: {DATA_DIR} is <repo>/data, {TEMP_DIR} is the test's own place in this run.
SERVICES = [duckdb_engine("duck", requires=["delta", "json"]), delta_spark()]
