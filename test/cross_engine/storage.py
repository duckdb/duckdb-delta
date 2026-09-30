"""The cross-engine round trip on the bound storage: DuckDB creates a table where nothing exists yet,
Spark reads and writes it, both engines read it back.
"""

from ducktest5 import test

# A case, in the order the test uses it.
#   columns, partition_by     the CREATE TABLE; no PARTITIONED BY when partition_by is empty
#   seed_values               what DuckDB inserts, and the rows every engine must read back
#   append_values             what Spark inserts later
CASES = {
    "plain": {
        "columns": "i INTEGER, s VARCHAR",
        "partition_by": "",
        "seed_values": [(1, "duck"), (2, "duck")],
        "append_values": [(3, "spark")],
    },
    # Partition values that must be encoded in the path.
    "partitioned_encoded": {
        "columns": "i INTEGER, p VARCHAR",
        "partition_by": "PARTITIONED BY (p)",
        "seed_values": [(1, "a b"), (2, "a/b"), (3, "a=b"), (4, "a%b"), (5, "ünï")],
        "append_values": [(6, "x y/z")],
    },
}


@test(engines=["duck", "spark"], params=CASES)
def duckdb_creates_spark_reads_and_writes(ctx):
    duck = ctx.client("duck")
    spark = ctx.client("spark")
    duck.setup("ATTACH '{TEMP_DIR}/t' AS t (TYPE delta)")
    case = ctx.params

    # -----------------------------------------------------------------------------
    # DuckDB creates the table where nothing exists yet
    #
    duck.oks(
        """
        CREATE TABLE t.t ({columns}) {partition_by};
        INSERT INTO t.t VALUES {seed_values};
        """,
        case,
    )

    # -----------------------------------------------------------------------------
    # Spark reads, then writes
    #
    spark.expects("SELECT * FROM delta.`{TEMP_DIR}/t` ORDER BY i", case["seed_values"])
    spark.oks("INSERT INTO delta.`{TEMP_DIR}/t` VALUES {append_values}", case)

    # -----------------------------------------------------------------------------
    # Both engines read what both wrote
    #
    duck.expects("SELECT * FROM t.t ORDER BY i", case["seed_values"] + case["append_values"])
    spark.expects("SELECT * FROM delta.`{TEMP_DIR}/t` ORDER BY i", case["seed_values"] + case["append_values"])
