"""Read Delta tables with Spark and print each row's variant columns as JSON.

For checking what Spark makes of tables DuckDB wrote:
    python scripts/data_generator/spark_read_variant.py <delta_table_path> [...]
"""

import sys

from generate_variant_data import spark_session


def main(paths):
    spark = spark_session()
    failed = False
    for path in paths:
        print(f"== {path}")
        try:
            df = spark.read.format("delta").load(path)
            cols = [f"to_json({f.name}) AS {f.name}" if f.dataType.typeName() != "integer" else f.name for f in df.schema.fields]
            for row in df.selectExpr(*cols).orderBy(df.schema.fields[0].name).collect():
                print(row.asDict())
        except Exception as e:
            failed = True
            print("ERROR:", str(e).splitlines()[0])
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main(sys.argv[1:])
