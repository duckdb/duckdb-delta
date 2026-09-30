"""A Delta table that DuckDB creates must be a table Spark can read AND write, and DuckDB must still
read and write it after Spark has. Spark is the judge of whether DuckDB wrote a correct table: it
validates the protocol, the table properties and every commit as it replays the log.

One case per table property set, so a failure names the feature that broke.
"""

from ducktest5 import test

PROPERTY_SETS = {
    "plain": {},
    "deletion_vectors": {"delta.enableDeletionVectors": "true"},
    "in_commit_timestamps": {"delta.enableInCommitTimestamps": "true"},
    "v2_checkpoint": {"delta.feature.v2Checkpoint": "supported"},
    "checkpoint_policy_v2": {"delta.checkpointPolicy": "v2"},
    "vacuum_protocol_check": {"delta.feature.vacuumProtocolCheck": "supported"},
    "column_mapping_name": {"delta.columnMapping.mode": "name"},
    # What Unity Catalog requires of a catalog-managed table, minus catalogManaged itself (needs a catalog).
    "uc_cmt_required": {
        "delta.enableDeletionVectors": "true",
        "delta.enableInCommitTimestamps": "true",
        "delta.feature.v2Checkpoint": "supported",
        "delta.checkpointPolicy": "v2",
        "delta.feature.vacuumProtocolCheck": "supported",
    },
}


def with_clause(properties):
    """WITH ('k' = 'v', ...), or nothing."""
    pairs = ", ".join(f"'{k}' = '{v}'" for k, v in properties.items())
    return f"WITH ({pairs})" if pairs else ""


def shown_by_spark(properties):
    """SHOW TBLPROPERTIES lists a property as set; a `delta.feature.*` key only changes the protocol."""
    return [(k, v) for k, v in properties.items() if not k.startswith("delta.feature.")]


@test(engines=["duck", "spark"], params=PROPERTY_SETS)
def duckdb_creates_then_spark_and_duckdb_interleave(ctx):
    path = ctx.location("t")
    duck = ctx.client("duck", table="t.t")
    spark = ctx.client("spark", table=f"delta.`{path}`")
    duck.setup(f"ATTACH '{path}' AS t (TYPE delta);")

    # -----------------------------------------------------------------------------
    # DuckDB creates
    #
    duck.oks(
        """
        CREATE TABLE {t} (i INTEGER, s VARCHAR) {with_properties};
        INSERT INTO {t} VALUES (1, 'duck'), (2, 'duck'), (3, 'duck');
        """,
        with_properties=with_clause(ctx.params),
    )

    # -----------------------------------------------------------------------------
    # Spark reads what DuckDB created, then writes
    #
    spark.expects(
        "SELECT i, s FROM {t} ORDER BY i",
        rows="""
        1	duck
        2	duck
        3	duck
        """,
    )
    spark.expects_contains("SHOW TBLPROPERTIES {t}", shown_by_spark(ctx.params))

    spark.oks("""
        INSERT INTO {t} VALUES (4, 'spark');
        DELETE FROM {t} WHERE i = 1;
        UPDATE {t} SET s = 'spark' WHERE i = 2;
        """)

    # -----------------------------------------------------------------------------
    # DuckDB reads what Spark wrote, then writes
    #
    duck.expects(
        "SELECT i, s FROM {t} ORDER BY i",
        rows="""
        2	spark
        3	duck
        4	spark
        """,
    )
    duck.oks("INSERT INTO {t} VALUES (5, 'duck')")

    # -----------------------------------------------------------------------------
    # Spark reads the table both engines wrote
    #
    spark.expects(
        "SELECT i, s FROM {t} ORDER BY i",
        rows="""
        2	spark
        3	duck
        4	spark
        5	duck
        """,
    )
