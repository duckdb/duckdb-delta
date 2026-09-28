"""Spark-written Delta tables with VARIANT columns, unshredded and shredded.

Standalone: python scripts/data_generator/generate_variant_data.py [BASE_PATH]
Needs pyspark + delta-spark 4.x; in a container set JAVA_TOOL_OPTIONS=-XX:-UseContainerSupport.
Each table lands in <BASE_PATH>/variant/<name>/delta_lake, with Spark's own read of it in
<BASE_PATH>/variant/<name>/spark_read.jsonl as the reference result.
"""

import json
import os
import shutil
import sys

from delta import configure_spark_with_delta_pip
from pyspark.sql import SparkSession

DEFAULT_BASE_PATH = os.path.dirname(os.path.realpath(__file__)) + "/../../data/generated"

SHRED_WRITE = "spark.sql.variant.writeShredding.enabled"
SHRED_FORCE = "spark.sql.variant.forceShreddingSchemaForTest"
SHRED_INFER = "spark.sql.variant.inferShreddingSchema"
VARIANT_STATS = "spark.databricks.delta.variantShredding.collectVariantDataSkippingStats"
VARIANT_STATS_FOOTER = "spark.databricks.delta.variantShredding.parseFooterForStats"
ANNOTATE = "spark.sql.parquet.variant.annotateLogicalType.enabled"

# (id, json) rows covering every value shape; ids are stable across tables so tests can share expectations.
SHAPES = [
    (1, None),
    (2, 'null'),
    (3, '42'),
    (4, '1234567890123'),
    (5, '12.34'),
    (6, '1.5e10'),
    (7, '"hello"'),
    (8, 'true'),
    (9, '{"a": 1, "b": "x"}'),
    (10, '[1, "two", true, null]'),
    (11, '{"a": {"b": [1, {"c": 2}]}, "d": []}'),
    (12, '{}'),
]

# Typed scalars that JSON cannot express; cast straight to VARIANT.
TYPED_SCALARS = [
    (20, "CAST(DATE'2024-01-15' AS VARIANT)"),
    (21, "CAST(TIMESTAMP'2024-01-15 10:30:00' AS VARIANT)"),
    (22, "CAST(TIMESTAMP_NTZ'2024-01-15 10:30:00' AS VARIANT)"),
    (23, "CAST(CAST(12.34 AS DECIMAL(10, 2)) AS VARIANT)"),
    (24, "CAST(CAST(1.5 AS DOUBLE) AS VARIANT)"),
    (25, "CAST(X'DEADBEEF' AS VARIANT)"),
]


def spark_session():
    builder = (
        SparkSession.builder.appName("variant_fixtures")
        .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
        .config("spark.sql.catalog.spark_catalog", "org.apache.spark.sql.delta.catalog.DeltaCatalog")
        .config("spark.driver.host", "127.0.0.1")
        .config("spark.ui.enabled", "false")
        .config("spark.sql.session.timeZone", "UTC")
        .config("spark.sql.shuffle.partitions", "1")
    )
    spark = configure_spark_with_delta_pip(builder).getOrCreate()
    spark.sparkContext.setLogLevel("ERROR")
    return spark


def set_confs(spark, confs):
    for key in (SHRED_WRITE, SHRED_FORCE, SHRED_INFER, VARIANT_STATS, VARIANT_STATS_FOOTER, ANNOTATE):
        spark.conf.unset(key)
    for key, value in confs.items():
        spark.conf.set(key, value)


def json_rows(rows):
    def lit(j):
        return "NULL" if j is None else "'" + j.replace("'", "''") + "'"

    return ", ".join(f"({i}, {lit(j)})" for i, j in rows)


def shape_select(rows=SHAPES, typed=TYPED_SCALARS):
    """Rows of (id, v, n.x, n.arr[v, v]) so a variant also appears nested in a struct and an array."""
    parts = [f"SELECT id, parse_json(j) AS v FROM VALUES {json_rows(rows)} AS t(id, j)"]
    parts += [f"SELECT {i} AS id, {expr} AS v" for i, expr in typed]
    union = " UNION ALL ".join(parts)
    return (
        "SELECT /*+ COALESCE(1) */ id, v, "
        f"named_struct('x', v, 'arr', CASE WHEN v IS NULL THEN NULL ELSE array(v, v) END) AS n FROM ({union})"
    )


def create(spark, path, columns, props):
    tblprops = ", ".join(f"'{k}' = '{v}'" for k, v in props.items())
    clause = f" TBLPROPERTIES ({tblprops})" if tblprops else ""
    spark.sql(f"CREATE TABLE delta.`{path}` ({columns}) USING delta{clause}")


def dump_spark_read(spark, table_dir, path):
    df = spark.read.format("delta").load(path)
    variant_cols = [f.name for f in df.schema.fields if f.dataType.simpleString() != "int"]
    cols = ", ".join(["id"] + [f"to_json({c}) AS {c}" for c in variant_cols])
    rows = spark.sql(f"SELECT {cols} FROM delta.`{path}` ORDER BY id").collect()
    with open(os.path.join(table_dir, "spark_read.jsonl"), "w") as f:
        for row in rows:
            f.write(json.dumps(row.asDict()) + "\n")


def table(spark, base, name, build):
    table_dir = os.path.join(base, "variant", name)
    if os.path.isdir(table_dir):
        return
    path = os.path.join(table_dir, "delta_lake")
    try:
        build(spark, path)
        set_confs(spark, {})
        dump_spark_read(spark, table_dir, path)
    except Exception:
        shutil.rmtree(table_dir, ignore_errors=True)
        raise


# Delta-Spark 4.4's shredding writer fails (AIOOBE in finalizeSchemaAndFlush) with 4+ top-level columns.
SHAPE_COLUMNS = "id INT, v VARIANT, n STRUCT<x: VARIANT, arr: ARRAY<VARIANT>>"
SHREDDING = {"delta.enableVariantShredding": "true"}


def unshredded(spark, path):
    set_confs(spark, {SHRED_WRITE: "false"})
    create(spark, path, SHAPE_COLUMNS, {})
    spark.sql(f"INSERT INTO delta.`{path}` {shape_select()}")


def shredded_object(spark, path):
    # Partially shredded: rows 9 and 11 hit fields outside the schema, 3/7 are not objects at all.
    set_confs(spark, {SHRED_WRITE: "true", SHRED_FORCE: "a int, b string, c struct<e: int>"})
    create(spark, path, SHAPE_COLUMNS, SHREDDING)
    extra = [
        (30, '{"a": 2, "b": "y", "z": true}'),
        (31, '{"a": "notint", "b": "w"}'),
        (32, '{"b": "only b"}'),
        (33, '{"a": 3, "c": {"e": 4, "f": 5}}'),
        (34, '{"a": null, "b": null}'),
    ]
    spark.sql(f"INSERT INTO delta.`{path}` {shape_select(SHAPES + extra)}")


def shredded_typed_fields(spark, path):
    # One typed_value per physical type; row 41 mismatches every field so all land in the residual value.
    schema = (
        "i8 tinyint, i16 smallint, i32 int, i64 bigint, f float, d double, dec4 decimal(4, 2), "
        "dec9 decimal(9, 3), dec18 decimal(18, 4), s string, bin binary, b boolean, dt date, "
        "ts timestamp, tsn timestamp_ntz"
    )
    set_confs(spark, {SHRED_WRITE: "true", SHRED_FORCE: schema})
    create(spark, path, "id INT, v VARIANT", SHREDDING)
    spark.sql(f"""INSERT INTO delta.`{path}` SELECT /*+ COALESCE(1) */ * FROM VALUES
        (40, to_variant_object(named_struct(
            'i8', CAST(1 AS TINYINT), 'i16', CAST(300 AS SMALLINT), 'i32', 70000, 'i64', 5000000000L,
            'f', CAST(1.5 AS FLOAT), 'd', CAST(2.25 AS DOUBLE), 'dec4', CAST(12.34 AS DECIMAL(4, 2)),
            'dec9', CAST(123456.789 AS DECIMAL(9, 3)), 'dec18', CAST(12345678901234.5678 AS DECIMAL(18, 4)),
            's', 'str', 'bin', X'CAFE', 'b', true, 'dt', DATE'2024-01-15',
            'ts', TIMESTAMP'2024-01-15 10:30:00', 'tsn', TIMESTAMP_NTZ'2024-01-15 10:30:00'))),
        (41, parse_json('{{"i8": "x", "i16": "x", "i32": "x", "i64": "x", "f": "x", "d": "x", "dec4": "x",
            "dec9": "x", "dec18": "x", "s": 1, "bin": 1, "b": 1, "dt": 1, "ts": 1, "tsn": 1}}')),
        (42, parse_json('{{}}')),
        (43, NULL)
        AS t(id, v)""")


def shredded_scalar(spark, path):
    set_confs(spark, {SHRED_WRITE: "true", SHRED_FORCE: "bigint"})
    create(spark, path, SHAPE_COLUMNS, SHREDDING)
    spark.sql(f"INSERT INTO delta.`{path}` {shape_select()}")


def shredded_array(spark, path):
    set_confs(spark, {SHRED_WRITE: "true", SHRED_FORCE: "array<int>"})
    create(spark, path, SHAPE_COLUMNS, SHREDDING)
    extra = [(50, '[1, 2, 3]'), (51, '[1, "x", 3]'), (52, '[]'), (53, '[null, 2]')]
    spark.sql(f"INSERT INTO delta.`{path}` {shape_select(SHAPES + extra)}")


def shredded_multi_schema(spark, path):
    # Three files whose physical layout of `v` differs: int-shredded, string-shredded, unshredded.
    create(spark, path, "id INT, v VARIANT", SHREDDING)
    for confs, rows in [
        ({SHRED_WRITE: "true", SHRED_FORCE: "a int"}, [(60, '{"a": 1}'), (61, '{"a": "s"}')]),
        ({SHRED_WRITE: "true", SHRED_FORCE: "a string"}, [(62, '{"a": "t"}'), (63, '{"a": 2}')]),
        ({SHRED_WRITE: "false"}, [(64, '{"a": 3}'), (65, '{"a": "u"}')]),
    ]:
        set_confs(spark, confs)
        spark.sql(
            f"INSERT INTO delta.`{path}` SELECT /*+ COALESCE(1) */ id, parse_json(j) FROM VALUES {json_rows(rows)} AS t(id, j)"
        )


def shredded_inferred(spark, path):
    # Spark's own schema inference, the path a real writer takes; two commits + checkpoint.
    set_confs(spark, {SHRED_WRITE: "true", SHRED_INFER: "true"})
    create(spark, path, "id INT, v VARIANT", {**SHREDDING, "delta.checkpointInterval": "2"})
    for lo, hi in [(0, 100), (100, 200)]:
        spark.sql(f"""INSERT INTO delta.`{path}` SELECT /*+ COALESCE(1) */ CAST(i AS INT),
            parse_json(format_string('{{"n": %d, "name": "n%d", "tags": ["t%d", "u"], "o": {{"p": %d.5}}}}', i, i, i % 3, i))
            FROM range({lo}, {hi}) AS r(i)""")


def shredded_variant_stats(spark, path):
    # Variant min/max land, Z85-encoded, in the add action's stats JSON. No checkpoint: Delta-Spark 4.4 fails
    # (AIOOBE) writing one for this table.
    set_confs(
        spark,
        {SHRED_WRITE: "true", SHRED_FORCE: "a int, b string", VARIANT_STATS: "true", VARIANT_STATS_FOOTER: "true"},
    )
    create(spark, path, "id INT, v VARIANT", SHREDDING)
    for rows in [[(70, '{"a": 1, "b": "x"}'), (71, '{"a": 5, "b": "y"}')], [(72, '{"a": 10, "b": "z"}')]]:
        spark.sql(
            f"INSERT INTO delta.`{path}` SELECT /*+ COALESCE(1) */ id, parse_json(j) FROM VALUES {json_rows(rows)} AS t(id, j)"
        )


def shredded_dv_colmap(spark, path):
    props = {
        **SHREDDING,
        "delta.columnMapping.mode": "name",
        "delta.enableDeletionVectors": "true",
    }
    set_confs(spark, {SHRED_WRITE: "true", SHRED_FORCE: "a int, b string"})
    create(spark, path, "id INT, v VARIANT", props)
    rows = [(80, '{"a": 1, "b": "x"}'), (81, '{"a": 2, "b": "y"}'), (82, '{"a": 3, "c": 1}')]
    spark.sql(
        f"INSERT INTO delta.`{path}` SELECT /*+ COALESCE(1) */ id, parse_json(j) FROM VALUES {json_rows(rows)} AS t(id, j)"
    )
    spark.sql(f"DELETE FROM delta.`{path}` WHERE id = 81")
    spark.sql(f"ALTER TABLE delta.`{path}` RENAME COLUMN v TO w")


def shredded_unannotated(spark, path):
    # Shredded files carry the Parquet VARIANT logical type by default; this one is a bare struct.
    set_confs(spark, {SHRED_WRITE: "true", SHRED_FORCE: "a int, b string", ANNOTATE: "false"})
    create(spark, path, "id INT, v VARIANT", SHREDDING)
    rows = [(90, '{"a": 1, "b": "x"}'), (91, '{"a": "s", "c": 2}'), (92, '7')]
    spark.sql(
        f"INSERT INTO delta.`{path}` SELECT /*+ COALESCE(1) */ id, parse_json(j) FROM VALUES {json_rows(rows)} AS t(id, j)"
    )


TABLES = {
    "spark_unshredded": unshredded,
    "spark_shredded_object": shredded_object,
    "spark_shredded_typed_fields": shredded_typed_fields,
    "spark_shredded_scalar": shredded_scalar,
    "spark_shredded_array": shredded_array,
    "spark_shredded_multi_schema": shredded_multi_schema,
    "spark_shredded_inferred": shredded_inferred,
    "spark_shredded_variant_stats": shredded_variant_stats,
    "spark_shredded_dv_colmap": shredded_dv_colmap,
    "spark_shredded_unannotated": shredded_unannotated,
}


def generate(base_path, spark=None):
    spark = spark or spark_session()
    for name, build in TABLES.items():
        table(spark, base_path, name, build)


if __name__ == "__main__":
    generate(os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BASE_PATH))
