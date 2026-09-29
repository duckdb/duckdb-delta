"""The cross-engine round trip, on each kind of storage: DuckDB creates a table, Spark reads and
writes it, DuckDB reads it back. On an object store the weak points differ from a local path: a table
that does not exist yet, paths that need encoding, an attach that must say it writes.
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
    # Partition values that must be encoded in the object key.
    "partitioned_encoded": {
        "columns": "i INTEGER, p VARCHAR",
        "partition_by": "PARTITIONED BY (p)",
        "seed_values": [(1, "a b"), (2, "a/b"), (3, "a=b"), (4, "a%b"), (5, "ünï")],
        "append_values": [(6, "x y/z")],
    },
}


@test(engines=["spark"], variants=["storage"], cases=CASES)
def duckdb_creates_spark_reads_and_writes(ctx):
    path = ctx.location("t")
    # A remote attach is read-only unless it says otherwise; a local one takes the option too.
    duck = ctx.duckdb(f"ATTACH '{path}' AS t (TYPE delta, READ_WRITE);", table="t.t")
    spark = ctx.engine("spark", table=f"delta.`{path}`")
    case = ctx.case
    all_rows = "SELECT * FROM {t} ORDER BY i"

    # -----------------------------------------------------------------------------
    # DuckDB creates the table where nothing exists yet
    #
    duck.oks(
        """
        CREATE TABLE {t} ({columns}) {partition_by};
        INSERT INTO {t} VALUES {seed_values};
        """,
        case,
    )

    # -----------------------------------------------------------------------------
    # Spark reads through s3a, then writes
    #
    spark.expects(all_rows, case["seed_values"])
    spark.oks("INSERT INTO {t} VALUES {append_values}", case)

    # -----------------------------------------------------------------------------
    # Both engines read what both wrote
    #
    duck.expects(all_rows, case["seed_values"] + case["append_values"])
    spark.expects(all_rows, case["seed_values"] + case["append_values"])
