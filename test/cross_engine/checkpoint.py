"""A checkpoint DuckDB writes must be a checkpoint Spark can start from. Spark reads the log from
`_last_checkpoint` onward, so once the commits before the checkpoint are deleted (what log cleanup
does) every row from an earlier commit came out of DuckDB's checkpoint file. Then Spark writes, DuckDB
checkpoints again over Spark's commits (deletion vectors included), and both engines read.

One case per table property set that changes what a checkpoint must carry.
"""

import glob
import os

from ducktest5 import test

PROPERTY_SETS = {
    "plain": {},
    "deletion_vectors": {"delta.enableDeletionVectors": "true"},
    "v2_checkpoint": {"delta.feature.v2Checkpoint": "supported"},
    "column_mapping_name": {"delta.columnMapping.mode": "name"},
    "uc_cmt_required": {
        "delta.enableDeletionVectors": "true",
        "delta.enableInCommitTimestamps": "true",
        "delta.feature.v2Checkpoint": "supported",
        "delta.feature.vacuumProtocolCheck": "supported",
    },
}


def with_clause(properties):
    pairs = ", ".join(f"'{k}' = '{v}'" for k, v in properties.items())
    return f"WITH ({pairs})" if pairs else ""


def checkpoint_file(path, version):
    return os.path.join(path, "_delta_log", f"{version:020d}.checkpoint.parquet")


def last_checkpoint_version(path):
    import json

    with open(os.path.join(path, "_delta_log", "_last_checkpoint")) as f:
        return json.load(f)["version"]


def delete_commits_before(path, version):
    """Log cleanup by hand, as Delta does it: commits before the checkpoint go, the checkpoint's own
    commit stays (Spark refuses a log segment without it). Everything before `version` is now known
    only through the checkpoint."""
    for f in glob.glob(os.path.join(path, "_delta_log", "*.json")):
        if int(os.path.basename(f).split(".")[0]) < version:
            os.remove(f)


@test(engines=["duck", "spark"], params=PROPERTY_SETS)
def duckdb_checkpoints_spark_reads_from_the_checkpoint(ctx):
    path = ctx.location("t")
    duck = ctx.client("duck", table="t.t")
    spark = ctx.client("spark", table=f"delta.`{path}`")
    duck.setup(f"ATTACH '{path}' AS t (TYPE delta);")

    # -----------------------------------------------------------------------------
    # DuckDB creates, writes 3 commits, checkpoints
    #
    duck.setup(
        """
        CREATE TABLE {t} (i INTEGER, s VARCHAR) {with_properties};
        INSERT INTO {t} VALUES (1, 'duck'), (2, 'duck');
        INSERT INTO {t} VALUES (3, 'duck');
        INSERT INTO {t} VALUES (4, 'duck');
        """,
        with_properties=with_clause(ctx.params),
    )
    duck.oks("CHECKPOINT t")

    assert last_checkpoint_version(path) == 3
    # The v2Checkpoint feature decides the shape: a V2 checkpoint carries a checkpointMetadata
    # action, a classic one does not. Both keep the classic file name.
    checkpoint_metadata_columns = "1" if "delta.feature.v2Checkpoint" in ctx.params else "0"
    duck.expects(
        "SELECT count(*) FROM parquet_schema('{checkpoint}') WHERE name = 'checkpointMetadata'",
        rows=checkpoint_metadata_columns,
        checkpoint=checkpoint_file(path, 3),
    )
    delete_commits_before(path, 3)

    # -----------------------------------------------------------------------------
    # Spark reads from DuckDB's checkpoint alone, then writes
    #
    spark.expects(
        "SELECT i, s FROM {t} ORDER BY i",
        rows="""
        1	duck
        2	duck
        3	duck
        4	duck
        """,
    )
    spark.setup("""
        INSERT INTO {t} VALUES (5, 'spark'), (6, 'spark');
        DELETE FROM {t} WHERE i IN (2, 6);
        """)

    # -----------------------------------------------------------------------------
    # DuckDB reads Spark's commits, checkpoints over them
    #
    after_spark = """
        1	duck
        3	duck
        4	duck
        5	spark
        """
    duck.expects("SELECT i, s FROM {t} ORDER BY i", rows=after_spark)
    duck.oks("CHECKPOINT t")

    assert last_checkpoint_version(path) == 5
    duck.expects(
        "SELECT count(*) FROM parquet_schema('{checkpoint}') WHERE name = 'checkpointMetadata'",
        rows=checkpoint_metadata_columns,
        checkpoint=checkpoint_file(path, 5),
    )
    delete_commits_before(path, 5)

    # -----------------------------------------------------------------------------
    # Both engines read from the second checkpoint alone
    #
    spark.expects("SELECT i, s FROM {t} ORDER BY i", rows=after_spark)
    duck.expects("SELECT i, s FROM {t} ORDER BY i", rows=after_spark)
