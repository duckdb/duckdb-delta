# timestamp_partition

Tables partitioned on a Delta `timestamp` column (DuckDB `TIMESTAMPTZ`), for
`test/sql/inlined/timestamp_partition_timezone.test` (#414). Same five rows in both:

| id | p (UTC) |
|---|---|
| 1 | 2023-06-01 00:00 |
| 2 | 2023-12-31 22:00 |
| 3 | 2024-01-01 00:00 |
| 4 | 2024-01-01 03:00 |
| 5 | NULL |

| Fixture | Writer | log `partitionValues` | directory name |
|---|---|---|---|
| `delta_rs` | deltalake 1.2.1 (`gen_deltars.py`) | `2024-01-01 00:00:00.000000` (UTC, no zone) | the same string |
| `spark_los_angeles` | pyspark 4.2.0 + delta-spark 4.4.1, session TimeZone `America/Los_Angeles` (`gen_spark.py`) | `2024-01-01T00:00:00.000000Z` | `p=2023-12-31 16%3A00%3A00` (Los Angeles wall clock) |

`delta_rs` is also partitioned by `n`, a `timestamp_ntz` holding the same wall clock as `p`.

The directory name carries no meaning under the Delta protocol, and `spark_los_angeles` shows
why: its directory names are in the writer's time zone and do not name the stored instant.

Spark's `.crc` files and `_staged_commits/` were removed after generation.
