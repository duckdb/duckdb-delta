"""DuckDB creates a table by naming its path with `location`, and Spark writes through that same path:
both engines then read the rows each wrote, and the property given on CREATE is in the first commit.
"""

from ducktest5 import test


@test(sessions=["duck", "spark"])
def duckdb_creates_by_path_spark_appends_by_path(ctx):
    path = ctx.location("t")
    duck = ctx.session("duck")
    spark = ctx.session("spark")

    # -----------------------------------------------------------------------------
    # DuckDB creates the table at the path and inserts a row
    #
    duck.setup("ATTACH '{path}' AS people (TYPE delta)", path=path)
    duck.oks(
        """
        CREATE TABLE people.people (id INTEGER, name VARCHAR)
        WITH (location = '{path}', 'delta.columnMapping.mode' = 'name');
        INSERT INTO people.people VALUES (1, 'duck');
        """,
        path=path,
    )

    # -----------------------------------------------------------------------------
    # Spark appends a row through the same path
    #
    spark.oks("INSERT INTO delta.`{path}` VALUES (2, 'spark')", path=path)

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
        "SELECT id, name FROM delta.`{path}` ORDER BY id",
        rows="""
        1	duck
        2	spark
        """,
        path=path,
    )

    # -----------------------------------------------------------------------------
    # The property is in the table's first commit
    #
    duck.expects(
        """
        SELECT json_extract_string(metaData.configuration, '$."delta.columnMapping.mode"')
        FROM read_json('{path}/_delta_log/00000000000000000000.json')
        WHERE metaData IS NOT NULL
        """,
        rows="name",
        path=path,
    )
