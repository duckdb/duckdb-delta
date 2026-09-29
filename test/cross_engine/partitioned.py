"""A partitioned table DuckDB creates must partition the same way for Spark: Spark sees the partition
columns, reads DuckDB's partition values back exactly (encoded path segments included), selects by
them, and writes new partitions that DuckDB reads. One case per partition column shape.
"""

from datetime import date

from ducktest5 import test

# A case, in the order the test uses it. `_values` are native Python values; they go into the INSERT
# as SQL literals, and they are also the rows every engine must read back.
#   columns, partition_by     the CREATE TABLE
#   seed_values               what DuckDB inserts at the start
#   filter, filter_ids        a WHERE on the partition columns, and the ids (column i) it selects
#   append_values             what Spark inserts later, into a partition DuckDB did not write
CASES = {
    "varchar": {
        "columns": "i INTEGER, p VARCHAR",
        "partition_by": "p",
        "seed_values": [(1, "a"), (2, "b"), (3, "a")],
        "filter": "p = 'a'",
        "filter_ids": [1, 3],
        "append_values": [(4, "c")],
    },
    "integer": {
        "columns": "i INTEGER, p INTEGER",
        "partition_by": "p",
        "seed_values": [(1, 10), (2, 20), (3, 10)],
        "filter": "p = 10",
        "filter_ids": [1, 3],
        "append_values": [(4, 30)],
    },
    "date": {
        "columns": "i INTEGER, p DATE",
        "partition_by": "p",
        "seed_values": [(1, date(2026, 1, 1)), (2, date(2026, 2, 1)), (3, date(2026, 1, 1))],
        "filter": "p = DATE '2026-01-01'",
        "filter_ids": [1, 3],
        "append_values": [(4, date(2026, 3, 1))],
    },
    "two_columns": {
        "columns": "i INTEGER, p VARCHAR, q INTEGER",
        "partition_by": "p, q",
        "seed_values": [(1, "a", 1), (2, "a", 2), (3, "b", 1)],
        "filter": "p = 'a' AND q = 2",
        "filter_ids": [2],
        "append_values": [(4, "b", 2)],
    },
    # Values that must be encoded in the partition path; the reader has to decode them the same way.
    "encoded_values": {
        "columns": "i INTEGER, p VARCHAR",
        "partition_by": "p",
        "seed_values": [(1, "a b"), (2, "a/b"), (3, "a=b"), (4, "a%b"), (5, "ünï")],
        "filter": "p = 'a/b'",
        "filter_ids": [2],
        "append_values": [(6, "x y/z")],
    },
    "null_value": {
        "columns": "i INTEGER, p VARCHAR",
        "partition_by": "p",
        "seed_values": [(1, "a"), (2, None)],
        "filter": "p IS NULL",
        "filter_ids": [2],
        "append_values": [(3, "b")],
    },
}


@test(sessions=["duck", "spark"], params=CASES)
def duckdb_creates_partitioned_spark_reads_and_writes(ctx):
    path = ctx.location("t")
    duck = ctx.session("duck", table="t.t")
    spark = ctx.session("spark", table=f"delta.`{path}`")
    duck.setup(f"ATTACH '{path}' AS t (TYPE delta);")
    case = ctx.params

    # -----------------------------------------------------------------------------
    # DuckDB creates and fills the partitions
    #
    duck.oks(
        """
        CREATE TABLE {t} ({columns}) PARTITIONED BY ({partition_by});
        INSERT INTO {t} VALUES {seed_values};
        """,
        case,
    )

    # -----------------------------------------------------------------------------
    # Spark sees the partitioning, reads every value back, selects by partition
    #
    table_detail = spark.record("DESCRIBE DETAIL {t}")
    assert table_detail["partitionColumns"] == case["partition_by"].split(", ")

    all_rows = "SELECT * FROM {t} ORDER BY i"
    filtered_ids = "SELECT i FROM {t} WHERE {filter} ORDER BY i"

    spark.expects(all_rows, case["seed_values"])
    spark.expects(filtered_ids, case["filter_ids"], case)

    # -----------------------------------------------------------------------------
    # Spark writes a partition of its own; DuckDB reads everything, selects by partition
    #
    spark.oks("INSERT INTO {t} VALUES {append_values}", case)

    duck.expects(all_rows, case["seed_values"] + case["append_values"])
    duck.expects(filtered_ids, case["filter_ids"], case)
