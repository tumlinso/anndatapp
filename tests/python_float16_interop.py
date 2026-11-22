"""Round-trip dense binary16 and reject CSR/CSC binary16 across AnnData backends."""
from __future__ import annotations

import asyncio
from importlib.metadata import version
import json
import os
from pathlib import Path
import subprocess
import sys
from uuid import uuid4


def install_restricted_runner_wakeup() -> None:
    if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") != "1":
        return

    class PollPolicy(asyncio.DefaultEventLoopPolicy):
        def new_event_loop(self):
            loop = super().new_event_loop()

            def tick() -> None:
                loop.call_later(0.010, tick)

            loop.call_later(0.010, tick)
            return loop

    asyncio.set_event_loop_policy(PollPolicy())


install_restricted_runner_wakeup()

import anndata as ad
import h5py
import numpy as np
from scipy import sparse
from zarr.codecs import BytesCodec
from zarr.storage import LocalStore
import zarr


DENSE_BITS = np.array(
    [
        [0x3800, 0xC000, 0x0001, 0x8000],
        [0x4280, 0x4800, 0x7EAB, 0x0400],
        [0xBC00, 0x0100, 0x7BFF, 0x7C00],
        [0x0000, 0x3555, 0xFC00, 0x7DCB],
    ],
    dtype=np.uint16,
)
FILL_BITS = np.uint16(0x3555)


def write_anndata(path: Path, backend: str, data: ad.AnnData) -> None:
    if backend == "h5ad":
        data.write_h5ad(path)
    else:
        with ad.settings.override(
            zarr_write_format=int(backend[-1]), auto_shard_zarr_v3=False
        ):
            data.write_zarr(LocalStore(str(path)), consolidate_metadata=False)


def open_zarr(path: Path, mode: str = "r"):
    return zarr.open_group(LocalStore(str(path)), mode=mode, use_consolidated=False)


def write_dense(path: Path, backend: str, variant: str) -> np.ndarray:
    values = DENSE_BITS.view(np.float16)
    data = ad.AnnData(values.copy())
    data.layers["half"] = values.copy()
    data.obsm["half"] = values.copy()
    data.raw = ad.AnnData(values.copy(), obs=data.obs.copy(), var=data.var.copy())
    write_anndata(path, backend, data)
    if variant == "dense":
        return DENSE_BITS

    if backend == "h5ad":
        with h5py.File(path, "r+") as group:
            attrs = dict(group["X"].attrs)
            del group["X"]
            group.create_dataset("X", data=values, dtype=">f2", chunks=(2, 2),
                                 compression="gzip").attrs.update(attrs)
        return DENSE_BITS

    group = open_zarr(path, "a")
    attrs = dict(group["X"].attrs)
    del group["X"]
    options = {}
    if backend == "zarr3":
        options = {"serializer": BytesCodec(endian="big"), "shards": (4, 4)}
    target = group.create_array(
        "X", shape=values.shape, dtype=">f2", chunks=(2, 2),
        fill_value=FILL_BITS.view(np.float16), **options,
    )
    target.attrs.update(attrs)
    target[:2, :2] = values[:2, :2]
    expected = np.full(DENSE_BITS.shape, FILL_BITS, dtype=np.uint16)
    expected[:2, :2] = DENSE_BITS[:2, :2]
    return expected


def write_sparse(path: Path, backend: str, layout: str) -> None:
    values = np.array([[0, 1], [2, 0]], dtype=np.float32)
    sparse_type = sparse.csr_matrix if layout == "csr" else sparse.csc_matrix
    write_anndata(path, backend, ad.AnnData(sparse_type(values)))

    # Construct on-disk sparse FP16 without requiring SciPy to materialize it.
    def replace_data(group) -> np.ndarray:
        assert group.attrs["encoding-type"] == f"{layout}_matrix"
        half = group["data"][:].astype(np.float16)
        del group["data"]
        return half

    if backend == "h5ad":
        with h5py.File(path, "r+") as root:
            group = root["X"]
            group.create_dataset("data", data=replace_data(group))
    else:
        group = open_zarr(path, "a")["X"]
        group.create_array("data", data=replace_data(group))


def assert_half_bits(values: np.ndarray, expected: np.ndarray) -> None:
    assert values.dtype.kind == "f" and values.dtype.itemsize == 2, values.dtype
    native_bits = values.astype(np.float16, copy=False).view(np.uint16)
    np.testing.assert_array_equal(native_bits, expected)


def zarr_v3_byte_order(codecs) -> str:
    for codec in codecs:
        if codec["name"] == "bytes":
            return codec["configuration"]["endian"]
        if codec["name"] == "sharding_indexed":
            return zarr_v3_byte_order(codec["configuration"]["codecs"])
    raise AssertionError("Float16 array has no bytes codec")


def assert_store(path: Path, backend: str, expected: np.ndarray, byte_order: str) -> None:
    def physical_bits(group):
        node = group["X"]
        assert node.attrs["encoding-type"] == "array"
        assert node.attrs["encoding-version"] == "0.2.0"
        assert_half_bits(node[:], expected)
        if backend != "zarr3":
            assert node.dtype.isnative == (byte_order == sys.byteorder), node.dtype

    if backend == "h5ad":
        with h5py.File(path, "r") as group:
            physical_bits(group)
        reopened = ad.read_h5ad(path)
    else:
        physical_bits(open_zarr(path))
        if backend == "zarr2":
            metadata = json.loads((path / "X" / ".zarray").read_text())
            assert metadata["dtype"] == ("<f2" if byte_order == "little" else ">f2")
        else:
            metadata = json.loads((path / "X" / "zarr.json").read_text())
            assert metadata["data_type"] == "float16"
            assert zarr_v3_byte_order(metadata["codecs"]) == byte_order
        reopened = ad.read_zarr(LocalStore(str(path)))

    assert_half_bits(reopened.X, expected)
    assert_half_bits(reopened.layers["half"], DENSE_BITS)
    assert_half_bits(reopened.obsm["half"], DENSE_BITS)
    assert reopened.raw is not None
    assert_half_bits(reopened.raw.X, DENSE_BITS)


def run_driver(driver: str, mode: str, backend: str, *paths: Path) -> None:
    result = subprocess.run(
        [driver, mode, backend, *map(str, paths)], text=True, capture_output=True, check=False
    )
    if result.returncode:
        raise AssertionError(
            f"C++ {backend} {mode} fixture {paths[0]} failed ({result.returncode}):\n"
            f"{result.stdout}{result.stderr}"
        )


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: python_float16_interop.py DRIVER h5ad|zarr2|zarr3 ARTIFACT_DIR")
    driver = str(Path(sys.argv[1]).resolve())
    backend = sys.argv[2]
    for package, pinned in (("anndata", "0.13.4"), ("h5py", "3.16.0"), ("zarr", "3.4.2")):
        if version(package) != pinned:
            raise SystemExit(f"expected {package} {pinned}, got {version(package)}")
    root = os.environ.get("ANNDATAPP_FLOAT16_INTEROP_ARTIFACT_DIR")
    base = Path(sys.argv[3]) if root is None else Path(root) / backend
    artifact_dir = base / str(uuid4())
    artifact_dir.mkdir(parents=True)
    suffix = ".h5ad" if backend == "h5ad" else ".zarr"

    for mode in ("dense", "foreign" if backend == "h5ad" else "fill"):
        source = artifact_dir / f"python-{mode}{suffix}"
        output = artifact_dir / f"native-eager-{mode}{suffix}"
        backed_output = artifact_dir / f"native-backed-{mode}{suffix}"
        expected = write_dense(source, backend, mode)
        order = sys.byteorder if mode == "dense" else "big"
        assert_store(source, backend, expected, order)
        run_driver(driver, mode, backend, source, output, backed_output)
        assert_store(output, backend, expected, order)
        assert_store(backed_output, backend, expected, order)

        updated = expected.copy()
        updated[3, 3] = 0x8001
        updated[2, :2] = [0x0002, 0xC555]
        assert_store(source, backend, updated, order)

    for layout in ("csr", "csc"):
        source = artifact_dir / f"python-{layout}{suffix}"
        output = artifact_dir / f"rejected-{layout}{suffix}"
        write_sparse(source, backend, layout)
        run_driver(driver, "sparse", backend, source, output)
        for rejected in (output, Path(f"{output}.h5ad"), Path(f"{output}.zarr")):
            if not rejected.exists():
                continue
            if rejected.suffix == ".h5ad":
                with h5py.File(rejected, "r") as root:
                    assert "X" not in root
            else:
                assert not (rejected / "X").exists()

    print(f"{backend} dense FP16 round trips and explicit CSR/CSC FP16 rejections passed")


if __name__ == "__main__":
    main()
