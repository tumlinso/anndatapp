#!/usr/bin/env python3
"""Generate small, deterministic AnnDataSet CSR matrix fixtures."""

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path

import h5py
import numpy as np
import snapatac2 as snap


ROWS = 8192
COLS = 2048
BYTE_LIMIT = 128 * 1024 * 1024
CASES = {
    "sparse-c1": {"components": 1, "nnz_per_row": 16, "step": 127, "modulus": 97},
    "sparse-c8": {"components": 8, "nnz_per_row": 16, "step": 127, "modulus": 97},
    "sparse-c32": {"components": 32, "nnz_per_row": 16, "step": 127, "modulus": 97},
    "higher-density-c8": {"components": 8, "nnz_per_row": 128, "step": 15, "modulus": 97},
    "gzip-c8": {
        "components": 8,
        "nnz_per_row": 16,
        "step": 127,
        "modulus": 97,
        "compression": "gzip",
    },
}


def attr(obj, kind, version):
    obj.attrs["encoding-type"] = kind
    obj.attrs["encoding-version"] = version


def strings(group, name, values):
    data = np.asarray(list(values), dtype=object)
    ds = group.create_dataset(name, data=data, dtype=h5py.string_dtype("utf-8"))
    attr(ds, "string-array", "0.2.0")


def table(parent, name, index):
    group = parent.create_group(name)
    attr(group, "dataframe", "0.2.0")
    group.attrs["_index"] = "_index"
    group.attrs["column-order"] = np.asarray([], dtype=h5py.string_dtype("utf-8"))
    strings(group, "_index", index)
    return group


def empty_maps(f):
    for name in ("obsm", "varm", "obsp", "varp", "layers", "uns"):
        attr(f.create_group(name), "dict", "0.1.0")


def file_base(path, start, rows):
    f = h5py.File(path, "x")
    attr(f, "anndata", "0.1.0")
    table(f, "obs", (f"cell-{i:05d}" for i in range(start, start + rows)))
    table(f, "var", (f"gene-{i:04d}" for i in range(COLS)))
    empty_maps(f)
    return f


def _dataset(group, name, shape, dtype, compression, chunk_size=None):
    options = {}
    if compression == "gzip":
        options = {"chunks": (min(chunk_size, shape[0]),), "compression": "gzip", "compression_opts": 1}
    return group.create_dataset(name, shape=shape, dtype=dtype, **options)


def write_component(path, start, rows, spec):
    nnz = spec["nnz_per_row"]
    compression = spec.get("compression", "none")
    with file_base(path, start, rows) as f:
        x = f.create_group("X")
        attr(x, "csr_matrix", "0.1.0")
        x.attrs["shape"] = np.asarray([rows, COLS], dtype=np.int64)
        data = _dataset(x, "data", (rows * nnz,), "<f4", compression, 4096)
        indices = _dataset(x, "indices", (rows * nnz,), "<i4", compression, 4096)
        indptr = _dataset(x, "indptr", (rows + 1,), "<i8", compression, 1024)
        for ds in (data, indices, indptr):
            attr(ds, "array", "0.2.0")

        positions = np.arange(nnz, dtype=np.int64)[None, :]
        row_block = 512
        for first in range(0, rows, row_block):
            count = min(row_block, rows - first)
            global_rows = np.arange(start + first, start + first + count, dtype=np.int64)[:, None]
            flat_start = first * nnz
            flat_end = (first + count) * nnz
            indices[flat_start:flat_end] = (spec["step"] * positions + global_rows % spec["modulus"]).astype(np.int32).ravel()
            data[flat_start:flat_end] = (1 + (global_rows + positions) % 31).astype(np.float32).ravel()
        indptr[:] = np.arange(rows + 1, dtype=np.int64) * nnz


def write_manifest(directory, component_files):
    previous_cwd = Path.cwd()
    try:
        os.chdir(directory)
        dataset = snap.AnnDataSet(
            [(f"part-{i:03d}", Path(filename)) for i, filename in enumerate(component_files)],
            filename=Path("collection.h5ads"),
            add_key="component",
            use_absolute_path=False,
            backend="hdf5",
        )
        dataset.close()
    finally:
        os.chdir(previous_cwd)


def verify_component(path, start, rows, spec):
    nnz = spec["nnz_per_row"]
    with h5py.File(path, "r") as f:
        if f.attrs.get("encoding-type") != "anndata":
            raise ValueError(f"{path}: missing H5AD root encoding")
        x = f["X"]
        if x.attrs.get("encoding-type") != "csr_matrix":
            raise ValueError(f"{path}: X is not encoded as CSR")
        if not np.array_equal(x.attrs["shape"], np.asarray([rows, COLS], dtype=np.int64)):
            raise ValueError(f"{path}: incorrect component shape")
        if x["data"].dtype != np.dtype("<f4") or x["indices"].dtype != np.dtype("<i4") or x["indptr"].dtype != np.dtype("<i8"):
            raise ValueError(f"{path}: unexpected native sparse widths")
        if x["data"].compression != ("gzip" if spec.get("compression") == "gzip" else None):
            raise ValueError(f"{path}: unexpected data compression")
        if x["indices"].compression != x["data"].compression or x["indptr"].compression != x["data"].compression:
            raise ValueError(f"{path}: sparse arrays have inconsistent compression")
        expected_indptr = np.arange(rows + 1, dtype=np.int64) * nnz
        if not np.array_equal(x["indptr"][:], expected_indptr):
            raise ValueError(f"{path}: incorrect row offsets")
        global_rows = np.arange(start, start + rows, dtype=np.int64)[:, None]
        positions = np.arange(nnz, dtype=np.int64)[None, :]
        expected_indices = (spec["step"] * positions + global_rows % spec["modulus"]).astype(np.int32).ravel()
        expected_data = (1 + (global_rows + positions) % 31).astype(np.float32).ravel()
        if not np.array_equal(x["indices"][:], expected_indices):
            raise ValueError(f"{path}: indices differ from the source formula")
        if not np.array_equal(x["data"][:], expected_data):
            raise ValueError(f"{path}: values differ from the source formula")
        if np.any(x["indices"][:] < 0) or np.any(x["indices"][:] >= COLS):
            raise ValueError(f"{path}: an index is out of bounds")


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def component_ranges(count):
    return [(ROWS * i // count, ROWS * (i + 1) // count) for i in range(count)]


def predicted_bytes():
    total = 0
    for spec in CASES.values():
        nnz = spec["nnz_per_row"]
        matrix = ROWS * nnz * 8 + (ROWS + 1) * 8
        total += matrix + spec["components"] * 256 * 1024 + 2 * 1024 * 1024
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path, help="new directory for the five collections")
    args = parser.parse_args()
    if snap.__version__ != "2.10.0":
        raise SystemExit(f"expected pinned SnapATAC2 2.10.0, found {snap.__version__}")
    upper = predicted_bytes()
    if upper > BYTE_LIMIT:
        raise SystemExit(f"predicted fixture bound {upper} exceeds {BYTE_LIMIT}")
    out = args.out.absolute()
    out.mkdir(parents=True, exist_ok=False)

    inventory_files = {}
    fixture_metadata = {}
    for name, spec in CASES.items():
        directory = out / name
        directory.mkdir()
        ranges = component_ranges(spec["components"])
        component_files = []
        for i, (start, end) in enumerate(ranges):
            filename = f"part-{i:03d}.h5ad"
            path = directory / filename
            write_component(path, start, end - start, spec)
            verify_component(path, start, end - start, spec)
            component_files.append(filename)

        if ranges[0][0] != 0 or ranges[-1][1] != ROWS or any(a[1] != b[0] for a, b in zip(ranges, ranges[1:])):
            raise ValueError(f"{name}: component ranges do not partition all rows")
        write_manifest(directory, component_files)
        manifest_path = directory / "collection.h5ads"
        reader = snap.read_dataset(manifest_path, mode="r")
        try:
            if tuple(reader.X.shape) != (ROWS, COLS):
                raise ValueError(f"{name}: native SnapATAC2 open has the wrong logical shape")
        finally:
            reader.close()

        compression = spec.get("compression", "none")
        fixture_metadata[name] = {
            "manifest": f"{name}/collection.h5ads",
            "components": spec["components"],
            "nnz_per_row": spec["nnz_per_row"],
            "compression": compression,
            "compression_options": ({
                "codec": "gzip",
                "level": 1,
                "data_chunk_values": 4096,
                "indices_chunk_values": 4096,
                "indptr_chunk_values": 1024,
            } if compression == "gzip" else None),
            "component_row_ranges": [[start, end] for start, end in ranges],
            "component_files": [f"{name}/{item}" for item in component_files],
            "matrix_formula": {
                "column": f"{spec['step']}*k + (r % {spec['modulus']})",
                "value": "1 + ((r + k) % 31)",
                "r_is_global_row": True,
                "k_range": [0, spec["nnz_per_row"]],
            },
            "native_dtypes": {"data": "float32", "indices": "int32", "indptr": "int64"},
        }

    metadata = {
        "schema_version": 1,
        "rows": ROWS,
        "cols": COLS,
        "matrix_format": "CSR",
        "fixtures": fixture_metadata,
        "formula": {
            "column": "step*k + (r % modulus)",
            "value": "1 + ((r + k) % 31)",
            "steps_by_nnz_per_row": {"16": 127, "128": 15},
            "row_modulus": 97,
            "value_modulus": 31,
            "global_row_numbers_across_components": True,
        },
        "selector_cases": "supplied separately in frozen case JSON/text by the campaign plan",
        "tool_versions": {
            "snapatac2": snap.__version__,
            "h5py": importlib.metadata.version("h5py"),
            "numpy": np.__version__,
        },
        "byte_limit": BYTE_LIMIT,
        "predicted_upper_bound_bytes": upper,
    }
    metadata_path = out / "metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")

    for path in sorted(p for p in out.rglob("*") if p.is_file()):
        inventory_files[path.relative_to(out).as_posix()] = {
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }
    payload_bytes = sum(entry["bytes"] for entry in inventory_files.values())
    inventory = {"bytes": payload_bytes, "limit": BYTE_LIMIT, "files": inventory_files}
    inventory_path = out / "inventory.json"
    inventory_path.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    total_bytes = sum(path.stat().st_size for path in out.rglob("*") if path.is_file())
    if total_bytes > BYTE_LIMIT:
        raise SystemExit(f"generated fixture set is {total_bytes} bytes, over limit {BYTE_LIMIT}; preserve it for inspection")
    print(json.dumps({
        "out": str(out),
        "fixture_count": len(CASES),
        "files_in_inventory": len(inventory_files),
        "payload_bytes": payload_bytes,
        "total_bytes_including_inventory": total_bytes,
        "byte_limit": BYTE_LIMIT,
        "metadata": str(metadata_path),
        "inventory": str(inventory_path),
    }, sort_keys=True))


if __name__ == "__main__":
    main()
