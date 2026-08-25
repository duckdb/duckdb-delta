# Column-mapping write fixtures

Hand-written Delta logs, used by `test/sql/inlined/column_mapping/write_field_ids.test`. Each is a
single `metaData` action whose `schemaString` carries `delta.columnMapping.physicalName` and
`delta.columnMapping.id` per field, plus a `protocol` action enabling the `columnMapping` feature.

**No data files.** The tables start empty, so every parquet file the test inspects is one it just
wrote — which is the point: the assertions are about what our writer emits.

| Fixture | Mode | Schema | Why it exists |
|---|---|---|---|
| `name_mode` | `name` | `id`, `code` | A reader matches physical names, so a file written with logical names reads back as NULL with no error. |
| `id_mode` | `id` | `id`, `code` | A reader matches parquet field ids and must refuse a file that has none, which makes the table unreadable rather than merely wrong. |
| `not_null_name_mode` | `name` | `id NOT NULL`, `code` | Constraints are keyed on the logical name while statistics arrive under the physical one. |
| `nested_name_mode` | `name` | `id`, `s STRUCT(x)` | The struct and its child each carry their own physical name and id, so the fixture is well formed; we only map top-level columns, so the write is refused. |
| `partitioned_name_mode` | `name` | `id`, `p` partitioned on `p` | Partition values are written under logical names, so a mapped partitioned write is refused for the same reason. |

Hand-written rather than generated because the field metadata above is the entire input to the write
path, and because there is no way here to generate with Spark and read back with DuckDB in one run.
Spark-generated equivalents live under `data/generated/simple_table_column_mapped*` but cannot stand
in: they were created with type widening, which also enables `checkConstraints`, `generatedColumns`,
`invariants` and `changeDataFeed`, and the kernel refuses to write a table carrying writer features it
does not support.

For evidence that another engine accepts the result, see `scripts/verify_column_mapping_roundtrip.py`.
