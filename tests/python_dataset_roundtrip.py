#!/usr/bin/env python3
"""Qualify AnnDataSet I/O against independently written SnapATAC2 fixtures."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

import h5py
import hdf5plugin
import numpy as np

from python_dataset import check, create


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("expected path to test_dataset")
    driver = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="anndatapp-h5ads-") as directory:
        root = Path(directory)
        source = create(root / "reference")
        with h5py.File(source, "r+") as file:
            table = file["uns/AnnDataSet"]
            table.attrs["extra_test_metadata"] = "table-marker"
            extra = table.create_dataset("extra", data=np.array([7, 9], dtype=np.int32))
            extra.attrs["encoding-type"] = "array"
            extra.attrs["encoding-version"] = "0.2.0"
            extra.attrs["extra_column_metadata"] = "column-marker"
            del table.attrs["column-order"]
            table.attrs.create("column-order", np.array(["keys", "file_path", "extra"], dtype=object),
                               dtype=h5py.string_dtype("utf-8"))
        source_hash = hashlib.sha256(source.read_bytes()).digest()
        target = root / "native"
        target.mkdir()
        copied = target / "copy.h5ads"
        created = target / "created.h5ads"
        subprocess.run([driver, "--interop", str(source), str(copied)], check=True)
        assert hashlib.sha256(source.read_bytes()).digest() == source_hash
        check(copied)
        with h5py.File(source) as original, h5py.File(copied) as result:
            before, after = original["uns/AnnDataSet"], result["uns/AnnDataSet"]
            assert list(after.attrs["column-order"]) == list(before.attrs["column-order"])
            assert after.attrs["extra_test_metadata"] == before.attrs["extra_test_metadata"]
            assert after["extra"].attrs["extra_column_metadata"] == before["extra"].attrs["extra_column_metadata"]
            np.testing.assert_array_equal(after["extra"][:], before["extra"][:])
        subprocess.run([driver, "--create", str(source.parent), str(created)], check=True)
        check(created)

        # SnapATAC2's default HDF5 compression uses filter 32015. Check both
        # the explicit missing-filter diagnostic and successful native decode.
        compressed_dir = root / "compressed"
        shutil.copytree(source.parent, compressed_dir)
        for path in (compressed_dir / "first.h5ad", compressed_dir / "second.h5ad"):
            with h5py.File(path, "r+") as file:
                group = file["X"]
                old = group["data"]
                values, attributes = old[:], dict(old.attrs)
                del group["data"]
                data = group.create_dataset("data", data=values, **hdf5plugin.Zstd(clevel=3))
                for key, value in attributes.items():
                    data.attrs[key] = value
        compressed = compressed_dir / "dataset.h5ads"
        env = os.environ.copy()
        env["HDF5_PLUGIN_PATH"] = str(root / "no-plugins")
        missing = subprocess.run(
            [driver, "--interop", str(compressed), str(target / "unavailable.h5ads")],
            env=env, text=True, capture_output=True,
        )
        if missing.returncode == 0 or "HDF5 filter unavailable: 32015" not in missing.stderr:
            raise RuntimeError("expected HDF5 unavailable-filter error; got: " + missing.stdout + missing.stderr)
        env["HDF5_PLUGIN_PATH"] = str(hdf5plugin.PLUGINS_PATH)
        filtered = target / "filtered.h5ads"
        subprocess.run([driver, "--interop", str(compressed), str(filtered)], env=env, check=True)
        subprocess.run([sys.executable, str(Path(__file__).with_name("python_dataset.py")),
                        "check", str(filtered)], env=env, check=True)
        fixtures = {
            "reference/manifest": source,
            "reference/first.h5ad": source.parent / "first.h5ad",
            "reference/second.h5ad": source.parent / "second.h5ad",
            "compressed/manifest": compressed,
            "compressed/first.h5ad": compressed_dir / "first.h5ad",
            "compressed/second.h5ad": compressed_dir / "second.h5ad",
            "native/copied.h5ads": copied,
            "native/created.h5ads": created,
            "native/filtered.h5ads": filtered,
        }
        print("SHA256 " + json.dumps({name: hashlib.sha256(path.read_bytes()).hexdigest()
                                      for name, path in fixtures.items()}, sort_keys=True))
    print("SnapATAC2 AnnDataSet round trips passed")


if __name__ == "__main__":
    main()
