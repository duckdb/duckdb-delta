# partition_encoding

Hand-written Delta logs for partition-value encoding tests. No data files: the tables start empty,
so every commit and every parquet file the tests inspect is one the test just wrote, which is what
makes assertions about our own output meaningful.

| Fixture | Shape | Used by |
|---|---|---|
| `string_partition` | `(id INT, code STRING, p STRING)` partitioned by `p`, empty | `write_null_string_partition.test`, `write_partition_value_uri_encoding.test` |

The partition column's type is the point. On a STRING column the empty string is a legitimate
value, so it cannot also stand for null without collapsing the two; on an integer column it can,
which is why integer-partitioned coverage cannot detect the difference.

A STRING column is the easiest place to carry characters that have to be percent-encoded into the
directory name, which is what the URI-encoding test exercises -- but not the only one: a timestamp
serializes with a space in it, so that column type reaches the same path.
