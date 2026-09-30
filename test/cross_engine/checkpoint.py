"""A checkpoint DuckDB writes must be a checkpoint Spark can start from. Spark reads the log from
`_last_checkpoint` onward, so once the commits before the checkpoint are deleted (what log cleanup
does) every row from an earlier commit came out of DuckDB's checkpoint file. Then Spark writes, DuckDB
checkpoints again over Spark's commits (deletion vectors included), and both engines read.

One case per table property set that changes what a checkpoint must carry.
"""

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


def delete_commits_before(log, version):
    """Log cleanup by hand, as Delta does it: commits before the checkpoint go, the checkpoint's own
    commit stays (Spark refuses a log segment without it). Everything before `version` is now known
    only through the checkpoint."""
    # Not glob: the test's directory name holds its case as `[name]`, which glob reads as a pattern.
    for name in os.listdir(log):
        if name.endswith(".json") and int(name.split(".")[0]) < version:
            os.remove(os.path.join(log, name))


@test(engines=["duck", "spark"], params=PROPERTY_SETS)
def duckdb_checkpoints_spark_reads_from_the_checkpoint(ctx):
    duck = ctx.client("duck")
    spark = ctx.client("spark")
    duck.setup("ATTACH '{TEMP_DIR}/t' AS t (TYPE delta)")
    log = os.path.join(ctx.session.values["temp_dir"], "t", "_delta_log")
    # The v2Checkpoint feature decides the shape: a V2 checkpoint carries a checkpointMetadata
    # action, a classic one does not. Both keep the classic file name.
    checkpoint_metadata_columns = "1" if "delta.feature.v2Checkpoint" in ctx.params else "0"

    # -----------------------------------------------------------------------------
    # DuckDB creates, writes 3 commits, checkpoints
    #
    duck.setup(
        """
        CREATE TABLE t.t (i INTEGER, s VARCHAR) {with_properties};
        INSERT INTO t.t VALUES (1, 'duck'), (2, 'duck');
        INSERT INTO t.t VALUES (3, 'duck');
        INSERT INTO t.t VALUES (4, 'duck');
        """,
        with_properties=with_clause(ctx.params),
    )
    duck.oks("CHECKPOINT t")

    duck.expects("SELECT version FROM read_json('{TEMP_DIR}/t/_delta_log/_last_checkpoint')", rows="3")
    duck.expects(
        """
        SELECT count(*) FROM parquet_schema('{TEMP_DIR}/t/_delta_log/00000000000000000003.checkpoint.parquet')
        WHERE name = 'checkpointMetadata'
        """,
        rows=checkpoint_metadata_columns,
    )
    delete_commits_before(log, 3)

    # -----------------------------------------------------------------------------
    # Spark reads from DuckDB's checkpoint alone, then writes
    #
    spark.expects(
        "SELECT i, s FROM delta.`{TEMP_DIR}/t` ORDER BY i",
        rows="""
        1	duck
        2	duck
        3	duck
        4	duck
        """,
    )
    spark.setup("""
        INSERT INTO delta.`{TEMP_DIR}/t` VALUES (5, 'spark'), (6, 'spark');
        DELETE FROM delta.`{TEMP_DIR}/t` WHERE i IN (2, 6);
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
    duck.expects("SELECT i, s FROM t.t ORDER BY i", rows=after_spark)
    duck.oks("CHECKPOINT t")

    duck.expects("SELECT version FROM read_json('{TEMP_DIR}/t/_delta_log/_last_checkpoint')", rows="5")
    duck.expects(
        """
        SELECT count(*) FROM parquet_schema('{TEMP_DIR}/t/_delta_log/00000000000000000005.checkpoint.parquet')
        WHERE name = 'checkpointMetadata'
        """,
        rows=checkpoint_metadata_columns,
    )
    delete_commits_before(log, 5)

    # -----------------------------------------------------------------------------
    # Both engines read from the second checkpoint alone
    #
    spark.expects("SELECT i, s FROM delta.`{TEMP_DIR}/t` ORDER BY i", rows=after_spark)
    duck.expects("SELECT i, s FROM t.t ORDER BY i", rows=after_spark)
