# delta-rs fixture for test/sql/inlined/timestamp_partition_timezone.test
import datetime as dt, shutil, sys
import pyarrow as pa
from deltalake import write_deltalake
out = sys.argv[1]
shutil.rmtree(out, ignore_errors=True)
utc = dt.timezone.utc
ts = [dt.datetime(2023, 6, 1, 0, 0, tzinfo=utc), dt.datetime(2023, 12, 31, 22, 0, tzinfo=utc),
      dt.datetime(2024, 1, 1, 0, 0, tzinfo=utc), dt.datetime(2024, 1, 1, 3, 0, tzinfo=utc), None]
write_deltalake(out, pa.table({
    "id": pa.array([1, 2, 3, 4, 5], pa.int32()),
    "p": pa.array(ts, pa.timestamp("us", tz="UTC")),
    "n": pa.array([t.replace(tzinfo=None) if t else None for t in ts], pa.timestamp("us")),
}), partition_by=["p", "n"])
