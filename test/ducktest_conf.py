import os
import tempfile

from ducktest5 import default
from ducktest5.engines.duckdb import duckdb_engine
from ducktest5.engines.spark import delta_spark
from ducktest5.resources import StorageDef

SERVICES = [duckdb_engine("duck", requires=["delta", "json"]), delta_spark()]

# Under the system temp directory, so a run writes nothing into the repo.
STORAGES = [StorageDef("local", os.path.join(tempfile.gettempdir(), "ducktest-delta"))]

RULES = [default(bind={"storage": "local"})]
