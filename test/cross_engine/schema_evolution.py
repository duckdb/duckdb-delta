"""Spark changes the schema of a table DuckDB created; DuckDB must read the old and the new rows
under the new schema, write a row of the new shape, and Spark must read that row back.

One case per kind of schema change.
"""

from ducktest5 import test

# A case, in the order the test uses it. The table starts as (i INTEGER, s VARCHAR) with two rows.
#   properties        table properties the change needs, given to CREATE TABLE
#   alters            the ALTER TABLE statements Spark runs
#   spark_values      what Spark inserts under the new schema
#   rows_after_alter  all rows, as every engine must read them after the change and Spark's insert
#   duck_values       what DuckDB inserts under the new schema
CASES = {
    "add_column": {
        "properties": {},
        "alters": ["ALTER TABLE delta.`{TEMP_DIR}/t` ADD COLUMNS (n INTEGER)"],
        "spark_values": [(3, "spark", 30)],
        "rows_after_alter": [(1, "duck", None), (2, "duck", None), (3, "spark", 30)],
        "duck_values": [(4, "duck", 40)],
    },
    "rename_column": {
        "properties": {"delta.columnMapping.mode": "name"},
        "alters": ["ALTER TABLE delta.`{TEMP_DIR}/t` RENAME COLUMN s TO who"],
        "spark_values": [(3, "spark")],
        "rows_after_alter": [(1, "duck"), (2, "duck"), (3, "spark")],
        "duck_values": [(4, "duck")],
    },
    "drop_column": {
        "properties": {"delta.columnMapping.mode": "name"},
        "alters": ["ALTER TABLE delta.`{TEMP_DIR}/t` DROP COLUMN s"],
        "spark_values": [3],
        "rows_after_alter": [1, 2, 3],
        "duck_values": [4],
    },
    "widen_type": {
        "properties": {},
        "alters": [
            "ALTER TABLE delta.`{TEMP_DIR}/t` SET TBLPROPERTIES ('delta.enableTypeWidening' = 'true')",
            "ALTER TABLE delta.`{TEMP_DIR}/t` ALTER COLUMN i TYPE BIGINT",
        ],
        "spark_values": [(3000000000, "spark")],
        "rows_after_alter": [(1, "duck"), (2, "duck"), (3000000000, "spark")],
        "duck_values": [(4000000000, "duck")],
    },
}


def with_clause(properties):
    pairs = ", ".join(f"'{k}' = '{v}'" for k, v in properties.items())
    return f"WITH ({pairs})" if pairs else ""


@test(engines=["duck", "spark"], params=CASES)
def spark_changes_the_schema_duckdb_reads_and_writes(ctx):
    duck = ctx.client("duck")
    spark = ctx.client("spark")
    duck.setup("ATTACH '{TEMP_DIR}/t' AS t (TYPE delta)")
    case = ctx.params

    # -----------------------------------------------------------------------------
    # DuckDB creates the table under its first schema
    #
    duck.setup(
        """
        CREATE TABLE t.t (i INTEGER, s VARCHAR) {with_properties};
        INSERT INTO t.t VALUES (1, 'duck'), (2, 'duck');
        """,
        with_properties=with_clause(case["properties"]),
    )

    # -----------------------------------------------------------------------------
    # Spark changes the schema and writes a row of the new shape
    #
    spark.oks(case["alters"] + ["INSERT INTO delta.`{TEMP_DIR}/t` VALUES {spark_values}"], params=case)
    spark.expects("SELECT * FROM delta.`{TEMP_DIR}/t` ORDER BY 1", rows=case["rows_after_alter"])

    # -----------------------------------------------------------------------------
    # DuckDB reads old and new rows under the new schema, then writes a row of the new shape
    #
    duck.expects("SELECT * FROM t.t ORDER BY 1", rows=case["rows_after_alter"])

    duck.oks("INSERT INTO t.t VALUES {duck_values}", params=case)

    # -----------------------------------------------------------------------------
    # Both engines read the row DuckDB wrote
    #
    spark.expects("SELECT * FROM delta.`{TEMP_DIR}/t` ORDER BY 1", rows=case["rows_after_alter"] + case["duck_values"])
    duck.expects("SELECT * FROM t.t ORDER BY 1", rows=case["rows_after_alter"] + case["duck_values"])
