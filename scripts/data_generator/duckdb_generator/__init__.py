"""Fixture generation through DuckDB's own delta CREATE TABLE.

Enabled with DELTA_FIXTURE_GENERATOR=duckdb; unset, the pyspark and delta-rs backends
are used exactly as before. Fixtures needing more than CREATE + INSERT (schema
evolution, deletion vectors, column mapping) always fall back to their original
backend, so the switch is safe to flip for the whole file.

Generation runs through the built duckdb binary rather than the pip `duckdb` module:
the extension is statically linked into it, so this exercises the code under test
instead of whatever release the module happens to be built against.

Note that fixtures produced here differ from the pyspark ones in protocol versions:
the pyspark backend sets minReaderVersion/minWriterVersion via TBLPROPERTIES, which
CREATE TABLE cannot express yet.
"""

import json
import os
import shutil
import subprocess

REPO_ROOT = os.path.dirname(os.path.realpath(__file__)) + "/../../.."

#! Fixture options that CREATE + INSERT cannot express yet
UNSUPPORTED_OPTIONS = ("queries", "delete_predicate", "mapping_mode", "domain_metadata_entries")

#! Appending across several partitions at once fails (duckdb-delta#334). Partitioned
#! fixtures keep their original backend until that is fixed; flip with
#! DELTA_FIXTURE_PARTITIONED=1 to try them.
PARTITIONED_WRITES = os.environ.get("DELTA_FIXTURE_PARTITIONED") == "1"

#! The extension only creates a table whose name matches the attached name
ATTACH_NAME = "gen"

BUILD_TYPES = ("debug", "release")


def duckdb_binary():
    """Path to the duckdb shell with the delta extension linked in.

    DUCKDB_BINARY names one outright. Otherwise BUILD_TYPE picks between the trees
    under build/, defaulting to whichever has been built -- debug first, since that
    is what the local flow produces, while CI builds release and should set
    BUILD_TYPE explicitly.
    """
    explicit = os.environ.get("DUCKDB_BINARY")
    if explicit:
        if not os.path.isfile(explicit):
            raise FileNotFoundError(f"DUCKDB_BINARY points at '{explicit}', which does not exist.")
        return explicit

    build_type = os.environ.get("BUILD_TYPE")
    candidates = [build_type] if build_type else list(BUILD_TYPES)
    for candidate in candidates:
        binary = f"{REPO_ROOT}/build/{candidate}/duckdb"
        if os.path.isfile(binary):
            return binary

    raise FileNotFoundError(
        f"No duckdb binary under build/{{{','.join(candidates)}}}/. Build the extension first, "
        f"or point DUCKDB_BINARY at one."
    )


def _run(sql, json_output=False):
    command = [duckdb_binary()]
    if json_output:
        command.append("-json")
    command += ["-c", sql]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"duckdb failed on:\n{sql}\n\n{result.stderr.strip()}")
    return result.stdout


def _script(setup, *statements):
    return "\n".join([setup] if setup else []) + "\n" + "\n".join(statements)


def _create_table_sql(columns, partition_column):
    definitions = ", ".join(f'"{c["column_name"]}" {c["column_type"]}' for c in columns)
    statement = f"CREATE TABLE {ATTACH_NAME}.{ATTACH_NAME} ({definitions})"
    if partition_column:
        statement += f' PARTITIONED BY ("{partition_column}")'
    return statement + ";"


def _write_delta_table(delta_path, setup, source, partition_column):
    """Create a delta table at `delta_path` and fill it from `source`.

    `setup` runs first and may define what `source` reads. Only DDL produces no
    result set, so the DESCRIBE is the sole output of its invocation.
    """
    columns = json.loads(_run(_script(setup, f"DESCRIBE {source};"), json_output=True) or "[]")
    if not columns:
        raise RuntimeError(f"Source produced no columns: {source}")

    _run(
        _script(
            setup,
            f"ATTACH '{delta_path}' AS {ATTACH_NAME} (TYPE delta);",
            _create_table_sql(columns, partition_column),
            f"INSERT INTO {ATTACH_NAME}.{ATTACH_NAME} {source};",
        )
    )


def _write_reference_parquet(reference_path, setup, source, partition_column):
    os.makedirs(reference_path, exist_ok=True)
    if partition_column:
        copy = f"COPY ({source}) TO '{reference_path}' (FORMAT parquet, PARTITION_BY {partition_column});"
    else:
        copy = f"COPY ({source}) TO '{reference_path}/data.parquet' (FORMAT parquet);"
    _run(_script(setup, copy))


def _generate(generated_path, setup, source, partition_column, reference_dir):
    # An unnormalized attach path silently corrupts the paths recorded in the log,
    # so hand the extension a clean one (duckdb-delta#268 territory).
    generated_path = os.path.normpath(generated_path)
    if os.path.isdir(generated_path):
        return

    try:
        _write_delta_table(f"{generated_path}/delta_lake", setup, source, partition_column)
        if reference_dir:
            _write_reference_parquet(f"{generated_path}/{reference_dir}", setup, source, partition_column)
    except:
        if os.path.isdir(generated_path):
            shutil.rmtree(generated_path)
        raise


def generate_test_data_duckdb(base_path, path, query, part_column=False, add_golden_table=True):
    """DuckDB equivalent of generate_test_data_delta_rs.

    :param query: statements that leave the contents in a table called 'test_table'
    """
    _generate(
        f"{base_path}/{path}",
        setup=query,
        source="SELECT * FROM test_table",
        partition_column=part_column or None,
        reference_dir="duckdb" if add_golden_table else None,
    )


def generate_test_data_duckdb_from_query(base_path, current_path, source, partition_column=None,
                                         reference_dir=None):
    """DuckDB equivalent of generate_test_data_pyspark's two table-creation modes."""
    _generate(
        f"{base_path}/{current_path}",
        setup=None,
        source=source,
        partition_column=partition_column,
        reference_dir=reference_dir,
    )


def install_duckdb_backend(generate_test_data_pyspark, generate_test_data_delta_rs):
    """Return the generator entry points to actually use.

    Unless DELTA_FIXTURE_GENERATOR=duckdb, the originals are handed straight back.
    """
    if os.environ.get("DELTA_FIXTURE_GENERATOR", "default") != "duckdb":
        return generate_test_data_pyspark, generate_test_data_delta_rs

    def pyspark_or_duckdb(base_path, name, current_path, input_path=None, base_query=None, **kwargs):
        unsupported = [option for option in UNSUPPORTED_OPTIONS if kwargs.get(option)]
        if kwargs.get("partition_column") and not PARTITIONED_WRITES:
            unsupported.append("partitioning")
        if unsupported:
            print(f"[fixtures] {current_path}: pyspark (needs {', '.join(unsupported)})")
            return generate_test_data_pyspark(base_path, name, current_path, input_path=input_path,
                                              base_query=base_query, **kwargs)

        print(f"[fixtures] {current_path}: duckdb")
        source = base_query.rstrip().rstrip(";") if input_path is None \
            else f"SELECT * FROM read_parquet('{input_path}')"
        # Only the parquet-sourced fixtures carry a reference copy, matching the pyspark backend.
        return generate_test_data_duckdb_from_query(
            base_path, current_path, source,
            partition_column=kwargs.get("partition_column"),
            reference_dir="parquet" if input_path is not None else None,
        )

    def delta_rs_or_duckdb(base_path, path, query, part_column=False, add_golden_table=True):
        if part_column and not PARTITIONED_WRITES:
            print(f"[fixtures] {path}: delta-rs (needs partitioning)")
            return generate_test_data_delta_rs(base_path, path, query, part_column, add_golden_table)

        print(f"[fixtures] {path}: duckdb")
        return generate_test_data_duckdb(base_path, path, query, part_column, add_golden_table)

    return pyspark_or_duckdb, delta_rs_or_duckdb


__all__ = [
    "generate_test_data_duckdb",
    "generate_test_data_duckdb_from_query",
    "install_duckdb_backend",
    "duckdb_binary",
]
