# Column-mapping fixtures

Hand-written Delta logs. Each has a `metaData` action whose `schemaString` carries
`delta.columnMapping.physicalName` and `delta.columnMapping.id` per field, plus a `protocol` action
enabling the `columnMapping` feature.

**Write fixtures start empty**: `write_field_ids.test` and `invalid_annotations.test` inspect parquet
files they just wrote, so the assertions are about what our writer emits. **Read fixtures carry one
data file**, laid out the way a foreign writer would, so the assertions are about how the reader
resolves it. The table says which is which.

| Fixture | Mode | Schema | Why it exists |
|---|---|---|---|
| `name_mode` | `name` | `id`, `code` | A reader matches physical names, so a file written with logical names reads back as NULL with no error. |
| `id_mode` | `id` | `id`, `code` | A reader matches parquet field ids and must refuse a file that has none, which makes the table unreadable rather than merely wrong. |
| `not_null_name_mode` | `name` | `id NOT NULL`, `code` | Constraints are keyed on the logical name while statistics arrive under the physical one. |
| `nested_name_mode` | `name` | `id`, `s STRUCT(x)` | The struct and its child each carry their own physical name and id, so the fixture is well formed; we only map top-level columns, so the write is refused. |
| `partitioned_name_mode` | `name` | `id`, `p` partitioned on `p` | Partition values are written under logical names, so a mapped partitioned write is refused for the same reason. |
| `name_mode_nested_ids` | `name` | `a`, `l BIGINT[]`, `m MAP` | Read fixture. The list and map carry `delta.columnMapping.nested.ids` as UniForm writes them; a name-mode reader must keep resolving their children by name, not by those ids. Read by `name_mode_nested_ids.test`. |
| `id_mode_logical_names` | `id` | `a`, `b` | Read fixture. The file names its columns `a`/`b` and carries field ids 1/2; only the ids connect file to schema. Read by `read_by_mode.test` and `filename_option.test`. |
| `id_mode_physical_names` | `id` | `a`, `b` | Read fixture. Spark's layout: physical column names and field ids both present. |
| `id_mode_nested_struct` | `id` | `nested_col STRUCT(x, y)` | Read fixture. Struct and children each carry an id and a physical name; the file keeps the logical names. |
| `id_mode_containers` | `id` | `a`, `l BIGINT[]`, `m MAP` | Read fixture. Spark's layout for containers: ids on the columns, none on the list element or map entries, no `nested.ids`. |
| `id_mode_containers_nested_ids` | `id` | `a`, `l BIGINT[]`, `m MAP` | Read fixture. The IcebergCompat layout: `nested.ids` in the log, matching ids on the file's children, logical column names. |
| `name_mode_physical_names` | `name` | `a`, `b` | Read fixture. The file names its columns by physical name, as name mode requires. |
| `invalid_id_null` | `name` | as `name_mode`, `id`'s id is `null` | A JSON `null` id must be an error, not a value. Kernel refuses it when loading the schema. |
| `invalid_id_string` | `name` | as `name_mode`, `id`'s id is `"1"` | A numeric-looking string is still not a number. Kernel refuses it too. |
| `invalid_nested_id_range` | `id` | `a`, `l BIGINT[]`, `m MAP`, one nested id above `INT32` max | Read fixture. A nested id that cannot be a parquet field id must be a readable error at bind, not an abort inside the kernel's schema callback. Read by `invalid_annotations.test`. |
| `invalid_physical_name_empty` | `name` | as `name_mode`, `id`'s physical name is `""` | Kernel accepts an empty physical name on read (delta-spark accepts them); it names no column, so DuckDB refuses it. |

Hand-written rather than generated because the field metadata above is the entire input to the write
path, and because there is no way here to generate with Spark and read back with DuckDB in one run.
Spark-generated equivalents live under `data/generated/simple_table_column_mapped*` but cannot stand
in: they were created with type widening, which also enables `checkConstraints`, `generatedColumns`,
`invariants` and `changeDataFeed`, and the kernel refuses to write a table carrying writer features it
does not support.

For evidence that another engine accepts the result, see `scripts/verify_column_mapping_roundtrip.py`.
