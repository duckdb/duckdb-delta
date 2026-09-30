"""DuckDB creates a table by naming its path with `location`, and Spark writes through that same path:
both engines then read the rows each wrote, and the property given on CREATE is in the first commit.
`location` must be the path DuckDB attached; any other path is refused.
"""

from ducktest5 import test


@test(engines=["duck", "spark"])
def duckdb_creates_by_path_spark_appends_by_path(ctx):
    duck = ctx.client("duck")
    spark = ctx.client("spark")

    duck.setup("ATTACH '{TEMP_DIR}/people' AS people (TYPE delta)")

    # -----------------------------------------------------------------------------
    # DuckDB refuses a location other than the attached path
    #
    duck.errs(
        "CREATE TABLE people.people (id INTEGER, name VARCHAR) WITH (location = '{TEMP_DIR}/elsewhere')",
        "can only create a table at the attached path",
    )

    # -----------------------------------------------------------------------------
    # DuckDB creates the table at the attached path and inserts a row
    #
    duck.oks(
        """
        CREATE TABLE people.people (id INTEGER, name VARCHAR)
        WITH (location = '{TEMP_DIR}/people', 'delta.columnMapping.mode' = 'name');
        INSERT INTO people.people VALUES (1, 'duck');
        """,
    )

    # -----------------------------------------------------------------------------
    # Spark appends a row through the same path
    #
    spark.oks("INSERT INTO delta.`{TEMP_DIR}/people` VALUES (2, 'spark')")

    # -----------------------------------------------------------------------------
    # Both engines read both rows
    #
    duck.expects(
        "SELECT id, name FROM people.people ORDER BY id",
        rows="""
        1	duck
        2	spark
        """,
    )
    spark.expects(
        "SELECT id, name FROM delta.`{TEMP_DIR}/people` ORDER BY id",
        rows="""
        1	duck
        2	spark
        """,
    )

    # -----------------------------------------------------------------------------
    # The property is in the table's first commit
    #
    duck.expects(
        """
        SELECT json_extract_string(metaData.configuration, '$."delta.columnMapping.mode"')
        FROM read_json('{TEMP_DIR}/people/_delta_log/00000000000000000000.json')
        WHERE metaData IS NOT NULL
        """,
        rows="name",
    )
