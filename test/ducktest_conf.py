import os
import tempfile

from ducktest5 import default
from ducktest5.engines.spark import delta_spark
from ducktest5.resources import SessionDef, StorageDef, spark_session

ENGINES = [delta_spark()]

# Under the system temp directory, so a run writes nothing into the repo.
STORAGES = [StorageDef("local", os.path.join(tempfile.gettempdir(), "ducktest-delta"))]

SESSIONS = [
    SessionDef("duck", requires=["delta", "json"]),
    spark_session("spark"),
]

RULES = [default(bind={"storage": "local"})]
