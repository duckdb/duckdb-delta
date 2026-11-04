# timestamp_partition_write

Tables Spark made, for `test/sql/main/writing/timestamp_partition_write.test`: INSERT from DuckDB into a table
partitioned on a Delta `timestamp` (DuckDB `TIMESTAMPTZ`) must write the same `partitionValues` string Spark
writes, and INSERT into a table partitioned on `binary` is refused.

Written by pyspark 4.2.0 + delta-spark 4.4.1 with session TimeZone `America/Los_Angeles` (`gen_spark.py <this
dir>`), one row each:

| Fixture | Schema | log `partitionValues` | directory |
|---|---|---|---|
| `spark_timestamp` | `id INT, p TIMESTAMP`, partitioned by `p` | `2024-01-01T00:00:00.000000Z` | `p=2023-12-31 16%3A00%3A00` (Los Angeles wall clock) |
| `spark_timestamp_mapped` | the same, `delta.columnMapping.mode = name` | the same, under `p`'s physical name | `L5/` |
| `spark_binary` | `id INT, b BINARY`, partitioned by `b` | `abc` (`X'616263'`) | `b=abc` |

Spark's `.crc` files and `_staged_commits/` were removed after generation.
