"""Restore the pinned Delta Kernel fixtures used by artifact tests."""

import hashlib
import io
from pathlib import Path
import re
import subprocess
import tarfile
import urllib.request


def main():
    root = Path(__file__).resolve().parent.parent
    kernel = root / "build/release/rust/src/delta_kernel"
    cmake = (root / "CMakeLists.txt").read_text()
    repository, tag = re.search(r'GIT_REPOSITORY "([^"]*delta-kernel-rs)"\s+GIT_TAG (\S+)', cmake).groups()
    if not kernel.exists():
        subprocess.run(
            [
                "git",
                "clone",
                "--depth=1",
                "--filter=blob:none",
                "--sparse",
                "--branch",
                tag,
                repository,
                str(kernel),
            ],
            check=True,
        )
        subprocess.run(
            ["git", "sparse-checkout", "set", "kernel/tests/data", "acceptance"],
            cwd=kernel,
            check=True,
        )

    dat = kernel / "acceptance/tests/dat"
    if (dat / ".done").exists():
        return
    # Match the corpus and checksum used by the pinned kernel's own build script.
    build_script = (kernel / "acceptance/build.rs").read_text()
    version = re.search(r'const DAT_VERSION: &str = "([^"]+)"', build_script)[1]
    checksum = re.search(r'const DAT_CHECKSUM: &str = "([^"]+)"', build_script)[1]
    url = f"https://github.com/delta-incubator/dat/releases/download/v{version}/" f"deltalake-dat-v{version}.tar.gz"
    with urllib.request.urlopen(url) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != checksum:
        raise ValueError("Delta acceptance test archive checksum mismatch")
    dat.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as bundle:
        bundle.extractall(dat, filter="data")
    (dat / ".done").write_text("done")


if __name__ == "__main__":
    main()
