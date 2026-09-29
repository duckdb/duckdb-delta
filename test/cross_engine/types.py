"""Every column type DuckDB can create must be the same type and hold the same values for Spark, in
both directions: Spark reads what DuckDB wrote, DuckDB reads what Spark wrote. One case per type.
"""

from ducktest5 import test

# A case, in the order the test uses it. The table is (id INTEGER, c <type>).
#   type          the column type DuckDB creates, as DuckDB's typeof() reports it
#   spark_type    the type Spark must see, as Spark's typeof() reports it
#   duck_value    the value, in DuckDB's SQL
#   spark_value   the same value, in Spark's SQL
#   compare_as    an expression over c that gives the same text in both engines; a dict where the
#                 engines need different SQL for it. Default: c itself.
#   reads_as      what compare_as must read for the value, in both engines
CASES = {
    "boolean": {
        "type": "BOOLEAN",
        "spark_type": "boolean",
        "duck_value": "true",
        "spark_value": "true",
        "reads_as": "true",
    },
    "tinyint": {
        "type": "TINYINT",
        "spark_type": "tinyint",
        "duck_value": "-128",
        "spark_value": "CAST(-128 AS TINYINT)",
        "reads_as": "-128",
    },
    "smallint": {
        "type": "SMALLINT",
        "spark_type": "smallint",
        "duck_value": "-32768",
        "spark_value": "CAST(-32768 AS SMALLINT)",
        "reads_as": "-32768",
    },
    "integer": {
        "type": "INTEGER",
        "spark_type": "int",
        "duck_value": "-2147483648",
        "spark_value": "-2147483648",
        "reads_as": "-2147483648",
    },
    "bigint": {
        "type": "BIGINT",
        "spark_type": "bigint",
        "duck_value": "-9223372036854775808",
        "spark_value": "-9223372036854775808L",
        "reads_as": "-9223372036854775808",
    },
    "float": {
        "type": "FLOAT",
        "spark_type": "float",
        "duck_value": "1.5",
        "spark_value": "CAST(1.5 AS FLOAT)",
        "reads_as": "1.5",
    },
    "double": {
        "type": "DOUBLE",
        "spark_type": "double",
        "duck_value": "-0.25",
        "spark_value": "-0.25D",
        "reads_as": "-0.25",
    },
    "varchar": {
        "type": "VARCHAR",
        "spark_type": "string",
        "duck_value": "'ünï ''quoted'''",
        "spark_value": "'ünï \\'quoted\\''",
        "reads_as": "ünï 'quoted'",
    },
    "blob": {
        "type": "BLOB",
        "spark_type": "binary",
        "duck_value": "'\\x00\\xFF'::BLOB",
        "spark_value": "X'00FF'",
        "compare_as": "hex(c)",
        "reads_as": "00FF",
    },
    "date": {
        "type": "DATE",
        "spark_type": "date",
        "duck_value": "DATE '2026-01-02'",
        "spark_value": "DATE '2026-01-02'",
        "reads_as": "2026-01-02",
    },
    # An instant: compared as microseconds since the epoch, so no session time zone takes part.
    "timestamp_tz": {
        "type": "TIMESTAMP WITH TIME ZONE",
        "spark_type": "timestamp",
        "duck_value": "TIMESTAMPTZ '2026-01-02 03:04:05.123456+00'",
        "spark_value": "TIMESTAMP '2026-01-02 03:04:05.123456+00:00'",
        "compare_as": {"duckdb": "epoch_us(c)", "spark": "unix_micros(c)"},
        "reads_as": "1767323045123456",
    },
    "timestamp_ntz": {
        "type": "TIMESTAMP",
        "spark_type": "timestamp_ntz",
        "duck_value": "TIMESTAMP '2026-01-02 03:04:05.123456'",
        "spark_value": "TIMESTAMP_NTZ '2026-01-02 03:04:05.123456'",
        "compare_as": "CAST(c AS STRING)",
        "reads_as": "2026-01-02 03:04:05.123456",
    },
    # As text, because a client that returns JSON numbers would round an 18-digit decimal.
    "decimal": {
        "type": "DECIMAL(18,4)",
        "spark_type": "decimal(18,4)",
        "duck_value": "12345678901234.5678",
        "spark_value": "12345678901234.5678BD",
        "compare_as": "CAST(c AS STRING)",
        "reads_as": "12345678901234.5678",
    },
    # A nested value reads as compact JSON in every engine.
    # A nested value reads as compact JSON in every engine.
    "struct": {
        "type": "STRUCT(a INTEGER, b VARCHAR)",
        "spark_type": "struct<a:int,b:string>",
        "duck_value": "{'a': 1, 'b': 'x'}",
        "spark_value": "named_struct('a', 1, 'b', 'x')",
        "reads_as": '{"a":1,"b":"x"}',
    },
    "list": {
        "type": "INTEGER[]",
        "spark_type": "array<int>",
        "duck_value": "[1, NULL, 3]",
        "spark_value": "array(1, NULL, 3)",
        "reads_as": "[1,null,3]",
    },
    "map": {
        "type": "MAP(VARCHAR, INTEGER)",
        "spark_type": "map<string,int>",
        "duck_value": "MAP {'k': 1}",
        "spark_value": "map('k', 1)",
        "reads_as": '{"k":1}',
    },
}


@test(sessions=["duck", "spark"], params=CASES)
def a_type_is_the_same_type_and_value_in_both_engines(ctx):
    path = ctx.location("t")
    duck = ctx.session("duck", table="t.t")
    spark = ctx.session("spark", table=f"delta.`{path}`")
    duck.setup(f"ATTACH '{path}' AS t (TYPE delta);")
    case = ctx.params

    compare_as = case.get("compare_as", "c")
    duck_c, spark_c = (compare_as["duckdb"], compare_as["spark"]) if isinstance(compare_as, dict) else (compare_as,) * 2
    value, null = case["reads_as"], "NULL"

    # -----------------------------------------------------------------------------
    # DuckDB creates the column and writes a value and a NULL
    #
    duck.oks(
        """
        CREATE TABLE {t} (id INTEGER, c {type});
        INSERT INTO {t} VALUES (1, {duck_value}), (2, NULL);
        """,
        case,
    )

    # -----------------------------------------------------------------------------
    # Spark sees the type and reads the value, then writes the same value
    #
    spark.expects("SELECT DISTINCT typeof(c) FROM {t}", [case["spark_type"]])
    spark.expects("SELECT id, {c}, c IS NULL FROM {t} ORDER BY id", [(1, value, False), (2, null, True)], c=spark_c)

    spark.oks("INSERT INTO {t} VALUES (3, {spark_value})", case)

    # -----------------------------------------------------------------------------
    # DuckDB still sees its type, and reads both engines' values as the same
    #
    both_wrote = [(1, value, False), (2, null, True), (3, value, False)]

    duck.expects("SELECT DISTINCT typeof(c) FROM {t}", [case["type"]])
    duck.expects("SELECT id, {c}, c IS NULL FROM {t} ORDER BY id", both_wrote, c=duck_c)
    spark.expects("SELECT id, {c}, c IS NULL FROM {t} ORDER BY id", both_wrote, c=spark_c)
