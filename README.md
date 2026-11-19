# DuckDB Delta Extension

This is the DuckDB extension for [Delta](https://delta.io/), built using the
[Delta Kernel](https://github.com/delta-incubator/delta-kernel-rs). The extension offers read and limited write (blind insert) support for delta
tables, both local and remote.

## Supported platforms

The supported platforms are:

- `linux_amd64` and `linux_amd64_gcc4` and `linux_arm64`
- `osx_amd64` and `osx_arm64`
- `windows_amd64`

Support for the [other](https://duckdb.org/docs/extensions/working_with_extensions#platforms) DuckDB platforms is
work-in-progress

## How to use

> [!NOTE]
> This extension requires the DuckDB v0.10.3 or higher

This extension is distributed as a binary extension. To use it, simply use one of its functions from DuckDB and the extension will be autoloaded:

```SQL
FROM delta_scan('s3://some/delta/table');
```

To scan a local table, use the full path prefixes with `file://`

```SQL
FROM delta_scan('file:///some/path/on/local/machine');
```

## Time travel

A table can be read at an earlier version, either by version number or by timestamp. A timestamp
selects the latest version committed at or before it. A timestamp carrying no zone -- a `TIMESTAMP`
rather than a `TIMESTAMPTZ`, or a string with no offset -- resolves through the session timezone:

```SQL
FROM delta_scan('file:///some/path', version => 3);
FROM delta_scan('file:///some/path', timestamp => TIMESTAMPTZ '2026-01-01 12:00:00+00');
```

The same applies to an attached table, both per query and for the whole attachment:

```SQL
ATTACH 'file:///some/path' AS tbl (TYPE delta);
FROM tbl AT (VERSION => 3);
FROM tbl AT (TIMESTAMP => TIMESTAMPTZ '2026-01-01 12:00:00+00');

ATTACH 'file:///some/path' AS tbl_old (TYPE delta, TIMESTAMP TIMESTAMPTZ '2026-01-01 12:00:00+00');
```

`version` and `timestamp` are mutually exclusive. An attached version or timestamp is the default for
the attachment rather than a pin, so a per-query `AT` clause overrides it.

A timestamp later than now is refused: it would name whichever commit happens to be last at the moment
of the read. `now()` itself is allowed, and reads the newest commit.

## Cloud Storage authentication

Note that using DuckDB [Secrets](https://duckdb.org/docs/configuration/secrets_manager.html) for Cloud authentication is supported.

### S3 Example

```SQL
CREATE SECRET (
  TYPE S3,
  PROVIDER CREDENTIAL_CHAIN
);
FROM delta_scan('s3://some/delta/table/with/auth');
```

### Azure Example

```SQL
CREATE SECRET (
    TYPE AZURE,
    PROVIDER CREDENTIAL_CHAIN,
    CHAIN 'cli',
    ACCOUNT_NAME 'mystorageaccount'
);
FROM delta_scan('abfss://some/delta/table/with/auth');
```

### GCS Example

<https://duckdb.org/docs/guides/network_cloud_storage/gcs_import.html>
You need to create [HMAC keys](https://console.cloud.google.com/storage/settings;tab=interoperability) and declare a secret.

```SQL
CREATE SECRET (
    TYPE GCS,
    KEY_ID 'xxxx',
    SECRET 'yyy'
);
```

## Features

Many features/optimizations are supported in this extension as it reuses most of DuckDB's
regular parquet scanning logic:

- multithreaded scans and parquet metadata reading
- data skipping/filter pushdown
  - skipping row-groups in file (based on parquet metadata)
  - skipping complete files (based on delta partition info)
- projection pushdown
- blind inserts
- time travel by version or timestamp
- scanning tables with deletion vectors
- all primitive types
- structs
- VARIANT type support
- reading Delta intervals (see [Intervals](#intervals)); writing them is not supported yet
- Cloud storage (AWS, Azure, GCP) support with secrets

## Intervals

Spark has two interval types, `interval year to month` (a number of months) and `interval day to
second` (an exact duration in microseconds), and writes them to Delta tables when its type check
is off. The Delta protocol itself defines no interval type. DuckDB has a single `INTERVAL` that
holds months, days and microseconds at once, and compares or converts a month as 30 days. A
Delta interval column reads as `INTERVAL_YEAR_MONTH` or `INTERVAL_DAY_TIME`: DuckDB `INTERVAL`
values that keep their kind, in a struct, list or map too.

```sql
SELECT ym, dt FROM delta_scan('file:///some/path');
-- 1 year 2 months    76:05:06.789123
```

- A year-month value holds months only, a day-time value microseconds only (`3 days 04:05:06`
  shows as `76:05:06`, and `date_part('day', dt)` is 0), as DuckDB imports any duration.
- Within one kind, comparison, ordering, grouping, `min` and `max` agree with Spark, as does
  calendar arithmetic such as `ts + ym`. `+`, `-` (also `add`, `subtract`) and unary minus keep
  the kind.
- A plain `INTERVAL` compared or combined with a Delta interval, or added to it, takes the
  column's kind and is refused when it does not fit: `ym = INTERVAL '30 days'` and
  `dt > INTERVAL '1 month'` fail instead of treating a month as 30 days. The two kinds do not
  compare, combine or add (`ym = dt`, `UNION`, `CASE`, `ym + dt`), as in Spark. A number is not
  an interval: `ym = 14` fails, as `INTERVAL '14 months' = 14` does.
- Casting to `INTERVAL` (`ym::INTERVAL`) opts into DuckDB's own `INTERVAL` semantics. Other
  functions that take an `INTERVAL` accept either kind and apply DuckDB's rules: `epoch_us`
  counts a year as 360 days and returns a `BIGINT`; multiplication and `avg` return a plain
  `INTERVAL` and can introduce days into a year-month value (`INTERVAL '1 month' * 1.5` is
  `1 month 15 days`). A `TIMESTAMPTZ + dt` across a daylight-saving change adds
  elapsed time, where Spark adds wall-clock time.
- `ym IN (SELECT ...)` and `ym = ANY (SELECT ...)` with a plain `INTERVAL` subquery fail with
  `Unimplemented type for cast`: DuckDB plans the subquery side with its built-in casts only.
  Cast one side (`ym::INTERVAL`, or the subquery's column to `INTERVAL_YEAR_MONTH`).
- The two types exist only while delta is loaded, so they cannot be stored in a DuckDB table:
  cast to `INTERVAL` first (`CREATE TABLE t AS SELECT ym::INTERVAL AS ym FROM ...`). DuckDB's own
  parquet writer stores `INTERVAL` at millisecond precision and refuses negative values.
- A window `RANGE` frame over an interval `ORDER BY` key takes a plain `INTERVAL` constant (or a
  string) as offset, and it must fit the key's kind (`ORDER BY ym RANGE BETWEEN INTERVAL '1 month'
  PRECEDING`); `INTERVAL '30 days'` there is refused, and so are a NULL offset, an offset typed as
  a kind, a column and a typed parameter. A key computed from an interval (`ts + dt`) follows its own type.
  For DuckDB's rules, cast the key to `INTERVAL` in a subquery and order by that column; a cast in
  the window's own `ORDER BY` is not enough.
- A filter on an interval partition column returns the right rows, but opens every file.
- Writing to a table with an interval column, and creating one, are refused for now.

## Building

See the [Extension Template](https://github.com/duckdb/extension-template) for generic build instructions

## Running tests

There are various tests available for the delta extension:

1. Delta Acceptence Test (DAT) based tests in `/test/sql/dat`
2. delta-kernel-rs based tests in `/test/sql/delta_kernel_rs`
3. Generated data based tests in `tests/sql/generated` (generated using [delta-rs](https://delta-io.github.io/delta-rs/), [PySpark](https://spark.apache.org/docs/latest/api/python/index.html), and DuckDB)

To run the first 2 sets of tests:

```shell
make test_debug
```

or in release mode

```shell
make test
```

To also run the tests on generated data:

```shell
make generate-data
GENERATED_DATA_AVAILABLE=1 make test
```

# Updating delta-kernel-rs / FFI version

Simply update the `GIT_TAG` definition found in `./CMakeLists.txt` and (re-)run
`make clean <debug|release>`. The FFI header is included directly from the
cargo build, and any breakage from the update should show up immediately.
