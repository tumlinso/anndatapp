#!/usr/bin/env python3
"""Prepare tiny family fixtures and run one Python family case per process.

Preparation is separate from case execution.  A case imports the pinned
reference packages before its phase clock, then reports open, requested-output,
and end-to-end durations in nanoseconds.  Use a separate authorized window for
each case and separate instrumented diagnostics from timing runs.
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import sys
import time
import warnings


SCRIPT_START_NS = time.perf_counter_ns()
LIMIT_BYTES = 10 * 1024 * 1024
N = 256
NNZ_PER_ROW = 8
ROWS = tuple(range(0, N, 4))
COLS = tuple(range(0, N, 4))
GOLDEN_CSR_SHA256 = "a8da79bc30f9c6f829175701edb00016bae0650b9cc7083b05e9248466dabb15"
PACKAGES = {
    "anndata": "0.13.4",
    "mudata": "0.4.1",
    "treedata": "0.4.0",
    "snapatac2": "2.10.0",
    "spatialdata": "0.9.0",
}
FIXTURES = {
    "anndata": "anndata.h5ad",
    "mudata": "mudata.h5mu",
    "treedata": "treedata.h5td",
    "dataset": "dataset.h5ads",
    "spatialdata": "spatialdata.zarr",
}
CASE_FAMILY = {
    "anndata": "anndata",
    "mudata": "mudata",
    "mudata-map": "mudata",
    "treedata": "treedata",
    "treedata-tree": "treedata",
    "dataset": "dataset",
    "spatialdata": "spatialdata",
    "spatialdata-raster": "spatialdata",
    "spatialdata-points": "spatialdata",
    "anndata-10k-control": "anndata",
    "dataset-10k-control": "dataset",
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_csr_parts(data, indices, indptr) -> tuple[bytes, bytes, bytes]:
    import numpy as np

    return (
        np.asarray(data, dtype="<f4").tobytes(order="C"),
        np.asarray(indices, dtype="<u4").tobytes(order="C"),
        np.asarray(indptr, dtype="<u8").tobytes(order="C"),
    )


def golden_csr() -> tuple[list[float], list[int], list[int]]:
    values: list[float] = []
    columns: list[int] = []
    offsets = [0]
    for row in ROWS:
        for k in range(NNZ_PER_ROW):
            values.append(float(1 + ((row + 3 * k) % 29)))
            columns.append(8 * k)
        offsets.append(len(values))
    return values, columns, offsets


def golden_csr_sha256() -> str:
    values, columns, offsets = golden_csr()
    data_bytes, index_bytes, offset_bytes = canonical_csr_parts(values, columns, offsets)
    return digest(data_bytes + index_bytes + offset_bytes)


def file_inventory(root: Path) -> dict[str, dict[str, int | str]]:
    entries: dict[str, dict[str, int | str]] = {}
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        with path.open("rb") as stream:
            sha = hashlib.file_digest(stream, "sha256").hexdigest()
        entries[rel] = {"bytes": path.stat().st_size, "sha256": sha}
    return entries


def package_versions() -> dict[str, str]:
    names = set(PACKAGES) | {"numpy", "pandas", "scipy", "h5py", "zarr", "pyarrow"}
    versions: dict[str, str] = {}
    for name in sorted(names):
        try:
            versions[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            versions[name] = "not-installed"
    return versions


def _install_zarr_wakeup_if_requested() -> None:
    if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") != "1":
        return
    repo = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(repo / "tests"))
    from python_storage import install_restricted_runner_wakeup

    install_restricted_runner_wakeup()


def _imports_for(case: str) -> dict[str, object]:
    # The restricted runner needs the same Zarr event-loop workaround used by
    # the existing independent interoperability checks.
    _install_zarr_wakeup_if_requested()

    import anndata as ad
    import numpy as np
    import pandas as pd
    import scipy.sparse as sp

    modules: dict[str, object] = {"ad": ad, "np": np, "pd": pd, "sp": sp}
    family = CASE_FAMILY[case]
    if family == "mudata":
        import mudata

        modules["mudata"] = mudata
    elif family == "treedata":
        import treedata

        modules["treedata"] = treedata
    elif family == "dataset":
        import snapatac2 as snap

        modules["snap"] = snap
    elif family == "spatialdata":
        import spatialdata

        modules["spatialdata"] = spatialdata
    return modules


class Observer:
    """Optional v1 interposers; these are diagnostics, never timing evidence."""

    def __init__(self, enabled: bool):
        self.enabled = enabled
        self.library = None
        self.io = (ctypes.c_uint64 * 16)()
        self.alloc = (ctypes.c_uint64 * 2)()
        if enabled:
            self.library = ctypes.CDLL(None)
            self.io_begin = self.library.v1_io_begin
            self.io_end = self.library.v1_io_end
            self.alloc_begin = self.library.v1_alloc_begin
            self.alloc_end = self.library.v1_alloc_end
            self.io_begin.argtypes = []
            self.io_begin.restype = None
            self.io_end.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
            self.io_end.restype = None
            self.alloc_begin.argtypes = []
            self.alloc_begin.restype = None
            self.alloc_end.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
            self.alloc_end.restype = None

    def begin(self) -> None:
        if self.enabled:
            self.io_begin()
            self.alloc_begin()

    def end(self) -> dict[str, object] | None:
        if not self.enabled:
            return None
        self.alloc_end(self.alloc)
        self.io_end(self.io)
        return {
            "scope": "open_through_requested_output",
            "intercepted_read_calls": int(self.io[0]),
            "intercepted_read_bytes": int(self.io[1]),
            "intercepted_write_calls": int(self.io[2]),
            "intercepted_write_bytes": int(self.io[3]),
            "h5dread_calls": int(self.io[14]),
            "h5dwrite_calls": int(self.io[15]),
            "cpp_new_calls": int(self.alloc[0]),
            "cpp_new_bytes": int(self.alloc[1]),
            "allocation_scope": (
                "global C++ operator new/new[] only; excludes Python and NumPy "
                "malloc allocators and does not count all process allocations"
            ),
            "io_scope": (
                "existing v1 intercepted libc/HDF5 calls; direct syscalls, mmap "
                "page-ins, hidden calls and physical device I/O are not counted"
            ),
            "instrumented_duration_is_timing_evidence": False,
        }


def _make_adata(start: int = 0, stop: int = N, *, matrix=None):
    import anndata as ad
    import numpy as np
    import pandas as pd
    import scipy.sparse as sp

    rows = np.arange(start, stop, dtype=np.int64)
    if matrix is None:
        indices = np.empty(len(rows) * NNZ_PER_ROW, dtype=np.int32)
        values = np.empty(len(rows) * NNZ_PER_ROW, dtype=np.float32)
        indptr = np.arange(len(rows) + 1, dtype=np.int64) * NNZ_PER_ROW
        k = np.arange(NNZ_PER_ROW, dtype=np.int32)
        for local, row in enumerate(rows):
            lo = local * NNZ_PER_ROW
            indices[lo:lo + NNZ_PER_ROW] = 32 * k + int(row % 4)
            values[lo:lo + NNZ_PER_ROW] = 1 + ((row + 3 * k) % 29)
        matrix = sp.csr_matrix((values, indices, indptr), shape=(len(rows), N), dtype=np.float32)
    obs_names = [f"cell-{i:03d}" for i in rows]
    var_names = [f"gene-{i:03d}" for i in range(N)]
    obs = pd.DataFrame(
        {
            "region": pd.Categorical(["cells"] * len(rows), categories=["cells"]),
            "instance_id": (rows + 1000).astype(np.int64),
        },
        index=pd.Index(obs_names, name="cell_id"),
    )
    var = pd.DataFrame(index=pd.Index(var_names, name="gene_id"))
    return ad.AnnData(X=matrix, obs=obs, var=var)


def _make_tree_graphs():
    import networkx as nx

    lineage = nx.DiGraph()
    lineage.add_node("root", rank=-1)
    for row in range(N):
        name = f"cell-{row:03d}"
        lineage.add_node(name, rank=row)
        lineage.add_edge("root", name, length=1.0 + row / 256.0)
    unused = nx.DiGraph()
    unused.add_node("unused-root", rank=-2)
    unused.add_node("cell-000", rank=0)
    unused.add_edge("unused-root", "cell-000", length=1.5)
    return {"lineage": lineage, "unused": unused}


def _write_fixture_set(root: Path) -> None:
    _install_zarr_wakeup_if_requested()
    import anndata as ad
    import mudata as md
    import numpy as np
    import pandas as pd
    import scipy.sparse as sp
    import snapatac2 as snap
    import spatialdata as sd
    from spatialdata.models import Image2DModel, Labels2DModel, PointsModel, TableModel
    from treedata import TreeData

    root.mkdir(parents=True, exist_ok=False)
    matrix_data = _make_adata()

    # AnnData primary control.
    matrix_data.write_h5ad(root / FIXTURES["anndata"], compression=None)

    # MuData: one modality makes the requested modality/map explicit and keeps
    # other modality payloads out of this case.
    mudata_value = md.MuData({"rna": matrix_data.copy()})
    mudata_value.write_h5mu(root / FIXTURES["mudata"], compression=None)

    # TreeData: one selected star topology plus a tiny unrelated named tree.
    tree_value = TreeData(
        X=matrix_data.X.copy(),
        obs=matrix_data.obs.copy(),
        var=matrix_data.var.copy(),
        obst=_make_tree_graphs(),
        label=None,
        alignment="leaves",
        allow_overlap=True,
    )
    tree_value.write_h5td(root / FIXTURES["treedata"], compression=None)

    # AnnDataSet: two relative H5AD component paths; selected rows span both.
    part_names = ("dataset-part-0.h5ad", "dataset-part-1.h5ad")
    matrix_data[: N // 2, :].copy().write_h5ad(root / part_names[0], compression=None)
    matrix_data[N // 2 :, :].copy().write_h5ad(root / part_names[1], compression=None)
    previous_cwd = Path.cwd()
    try:
        os.chdir(root)
        dataset = snap.AnnDataSet(
            [("part-0", Path(part_names[0])), ("part-1", Path(part_names[1]))],
            filename=Path(FIXTURES["dataset"]),
            add_key="component",
            use_absolute_path=False,
            backend="hdf5",
        )
        dataset.close()
    finally:
        os.chdir(previous_cwd)

    # SpatialData: exactly one target in each selected element family.
    table_data = matrix_data.copy()
    table_data.obs["region"] = "cells"
    table = TableModel.parse(
        table_data, region="cells", region_key="region", instance_key="instance_id"
    )
    y = np.arange(N, dtype=np.uint32)[:, None]
    x = np.arange(N, dtype=np.uint32)[None, :]
    pixels = (y * N + x).astype(np.uint16, copy=False)[None, :, :]
    image = Image2DModel.parse(
        pixels, dims=("c", "y", "x"), c_coords=["DAPI"], scale_factors=[1]
    )
    label_pixels = np.zeros((N, N), dtype=np.uint16)
    label_pixels[np.arange(N), np.arange(N)] = 1000 + np.arange(N, dtype=np.uint16)
    labels = Labels2DModel.parse(label_pixels, dims=("y", "x"))
    point_rows = np.arange(N, dtype=np.int64)
    point_frame = pd.DataFrame(
        {
            "x": point_rows.astype(np.float64) + 0.25,
            "y": 2.0 * point_rows.astype(np.float64) + 0.5,
            "spot_id": 1000 + point_rows,
        }
    )
    points = PointsModel.parse(point_frame, instance_key="spot_id")
    spatial = sd.SpatialData(images={"image": image}, labels={"cells": labels},
                             points={"points": points}, tables={"table": table})
    spatial.write(root / FIXTURES["spatialdata"], overwrite=False)


def prepare(out: Path) -> dict[str, object]:
    out = out.resolve()
    if out.exists():
        raise FileExistsError(f"refusing to overwrite existing fixture destination: {out}")
    # Matrix payloads dominate this estimate. The fixed allowance covers
    # metadata, axes, small trees, Zarr metadata and Parquet sidecars.
    payload = N * NNZ_PER_ROW * (4 + 4) + (N + 1) * 8
    upper_bound = 5 * payload + 4 * 1024 * 1024
    if upper_bound > LIMIT_BYTES:
        raise RuntimeError(f"fixture upper bound {upper_bound} exceeds {LIMIT_BYTES}")
    _write_fixture_set(out)
    files = file_inventory(out)
    physical_bytes = sum(int(info["bytes"]) for info in files.values())
    if physical_bytes > LIMIT_BYTES:
        raise RuntimeError(
            f"generated fixture bytes {physical_bytes} exceed {LIMIT_BYTES}; preserve for inspection"
        )
    if golden_csr_sha256() != GOLDEN_CSR_SHA256:
        raise RuntimeError("internal golden CSR digest changed")
    versions = package_versions()
    for name, expected in PACKAGES.items():
        if versions.get(name) != expected:
            raise RuntimeError(f"expected {name}=={expected}, found {versions.get(name)}")
    expected = {
        "matrix": {
            "shape": [64, 64],
            "nnz": 512,
            "value_dtype": "float32",
            "canonical_index_dtype": "uint32",
            "canonical_offset_dtype": "uint64",
            "sha256": GOLDEN_CSR_SHA256,
            "sha256_input": "data little-endian float32 || indices little-endian uint32 || indptr little-endian uint64",
        },
        "selector": {
            "rows": list(ROWS),
            "columns": list(COLS),
            "description": "absolute positions 0,4,...,252 on both axes",
        },
        "mudata_map": {
            "modality": "rna",
            "selected_obs_map": list(range(1, 65)),
            "selected_var_map": list(range(1, 65)),
        },
        "treedata_tree": {
            "names": ["lineage", "unused"],
            "key": "lineage",
            "selected_nodes": ["root", *[f"cell-{i:03d}" for i in ROWS]],
            "selected_ranks": [-1, *ROWS],
            "selected_edges": 64,
            "selected_edge_lengths": [1.0 + i / 256.0 for i in ROWS],
            "full_sha256": _tree_golden_sha256(tuple(range(N))),
            "selected_sha256": _tree_golden_sha256(ROWS),
        },
        "spatialdata_raster": {
            "element": "image",
            "dtype": "uint16",
            "source_shape": [1, 256, 256],
            "window": {"c": [0, 1, 1], "y": [16, 80, 2], "x": [32, 96, 2]},
            "shape": [1, 32, 32],
            "sha256": digest(
                bytes(
                    b"".join(
                        (
                            int(256 * row + col).to_bytes(2, "little")
                            for row in range(16, 80, 2)
                            for col in range(32, 96, 2)
                        )
                    )
                )
            ),
        },
        "spatialdata_points": {
            "element": "points",
            "first": 16,
            "count": 8,
            "columns": ["x", "y", "spot_id"],
            "sha256": _points_golden_sha256(),
        },
        "spatialdata_table_region": "cells (matching the labels element); table matrix X carries the common CSR fixture",
    }
    manifest = {
        "schema": "anndatapp.format_family_fixture.v1",
        "root": str(out),
        "limit_bytes": LIMIT_BYTES,
        "physical_bytes": physical_bytes,
        "conservative_upper_bound_bytes": upper_bound,
        "package_versions": versions,
        "fixture_paths": FIXTURES | {"dataset_part_0": "dataset-part-0.h5ad", "dataset_part_1": "dataset-part-1.h5ad"},
        "expected": expected,
        "files": files,
    }
    manifest_path = out / "inventory.json"
    with manifest_path.open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")
    readback = validate_fixture_set(out)
    print(
        json.dumps(
            {
                "status": "fixtures_prepared",
                "inventory": str(manifest_path),
                "physical_bytes": physical_bytes,
                "files": len(files),
                "matrix_golden_sha256": GOLDEN_CSR_SHA256,
                "fixture_readback": readback,
            },
            sort_keys=True,
        )
    )
    return manifest


def _load_manifest(root: Path) -> dict[str, object]:
    path = root / "inventory.json"
    if not path.is_file():
        raise FileNotFoundError(f"fixture inventory not found: {path}")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "anndatapp.format_family_fixture.v1":
        raise RuntimeError(f"unsupported fixture inventory schema: {manifest.get('schema')}")
    if manifest.get("expected", {}).get("matrix", {}).get("sha256") != GOLDEN_CSR_SHA256:
        raise RuntimeError("fixture inventory golden matrix digest does not match this driver")
    return manifest


def _to_memory(value):
    if hasattr(value, "compute"):
        value = value.compute()
    if hasattr(value, "to_memory"):
        value = value.to_memory()
    return value


def _select_csr(matrix, modules: dict[str, object], rows=ROWS, cols=COLS):
    np = modules["np"]
    sp = modules["sp"]
    block = matrix[list(rows), :]
    selected = block[:, list(cols)]
    selected = _to_memory(selected)
    if not sp.issparse(selected):
        selected = sp.csr_matrix(np.asarray(selected))
    selected = selected.tocsr(copy=False)
    selected.sum_duplicates()
    selected.sort_indices()
    if selected.shape != (64, 64):
        raise RuntimeError(f"selected CSR shape is {selected.shape}, expected (64, 64)")
    return selected


def _assert_csr(actual, modules: dict[str, object], golden=None) -> dict[str, object]:
    np = modules["np"]
    if golden is None:
        want_values, want_indices, want_offsets = golden_csr()
        want_sha = GOLDEN_CSR_SHA256
    else:
        want_values, want_indices, want_offsets, want_sha = golden
    got_values = np.asarray(actual.data, dtype="<f4")
    got_indices = np.asarray(actual.indices, dtype="<u4")
    got_offsets = np.asarray(actual.indptr, dtype="<u8")
    if got_values.tolist() != want_values:
        raise RuntimeError("selected CSR values differ from independent formula")
    if got_indices.tolist() != want_indices:
        raise RuntimeError("selected CSR indices differ from independent formula")
    if got_offsets.tolist() != want_offsets:
        raise RuntimeError("selected CSR offsets differ from independent formula")
    parts = canonical_csr_parts(got_values, got_indices, got_offsets)
    actual_sha = digest(b"".join(parts))
    if actual_sha != want_sha:
        raise RuntimeError(f"selected CSR SHA256 {actual_sha} != {want_sha}")
    return {
        "shape": [64, 64],
        "nnz": int(actual.nnz),
        "value_dtype": "float32",
        "canonical_index_dtype": "uint32",
        "canonical_offset_dtype": "uint64",
        "sha256": actual_sha,
        "golden": "pass",
        "hash_source": "canonical CSR output checked elementwise against formula",
    }


def _legacy_spread_positions(extent: int) -> list[int]:
    count = min(extent, 64)
    if count == 0:
        return []
    if count == 1:
        return [(extent - 1) // 2]
    distance = extent - 1
    denominator = count - 1
    quotient, remainder = divmod(distance, denominator)
    return [quotient * i + (remainder * i) // denominator for i in range(count)]


def _legacy_10k_golden(rows: list[int], cols: list[int]) -> tuple[list[float], list[int], list[int], str]:
    # Independent reconstruction of tools/format-family-screen/generate.py's
    # 10k CSR fixture; this is a read-only control over already existing files.
    column_positions = {column: i for i, column in enumerate(cols)}
    values: list[float] = []
    indices: list[int] = []
    offsets = [0]
    for row in rows:
        found = []
        for k in range(100):
            source_column = 199 * k + row % 97
            position = column_positions.get(source_column)
            if position is not None:
                found.append((position, float(1 + ((row + k) % 31))))
        found.sort()
        indices.extend(position for position, _value in found)
        values.extend(value for _position, value in found)
        offsets.append(len(values))
    parts = canonical_csr_parts(values, indices, offsets)
    return values, indices, offsets, digest(b"".join(parts))


def _legacy_10k_case(case: str, root: Path, modules: dict[str, object], diagnostic: bool) -> dict[str, object]:
    is_dataset = case == "dataset-10k-control"
    directory = root / "10000" if (root / "10000").is_dir() else root
    path = directory / ("collection.h5ads" if is_dataset else "monolithic.h5ad")
    rows = _legacy_spread_positions(10_000)
    cols = _legacy_spread_positions(20_000)
    expected = _legacy_10k_golden(rows, cols)
    observer = Observer(diagnostic)
    observer.begin()
    start = time.perf_counter_ns()
    if is_dataset:
        data = modules["snap"].read_dataset(path, mode="r")
    else:
        data = modules["ad"].read_h5ad(path, backed="r")
    opened = time.perf_counter_ns()
    selected = _select_csr(data.X, modules, rows, cols)
    finished = time.perf_counter_ns()
    diag = observer.end()
    result = _assert_csr(selected, modules, expected)
    _close(data, "dataset" if is_dataset else "anndata")
    return {
        "case": case,
        "fixture": str(path.resolve()),
        "fixture_source": "existing build/format-family-repair/screen/fixtures/10000; no new data generated",
        "selector": "64 C++ spread positions across 10k rows and 20k columns",
        "shape": [64, 64],
        "nnz": result["nnz"],
        "value_dtype": result["value_dtype"],
        "canonical_index_dtype": result["canonical_index_dtype"],
        "canonical_offset_dtype": result["canonical_offset_dtype"],
        "sha256": result["sha256"],
        "golden": result["golden"],
        "hash_source": "existing fixture formula reconstructed independently",
        "open_ns": opened - start,
        "prepare_and_allocate_ns": 0,
        "operation_ns": finished - opened,
        "end_to_end_ns": finished - start,
        "operation_comparable_to_cpp_caller_buffer": False,
        "comparison_basis": "same selected CSR output; compare end-to-end only",
        "api_boundary": "existing monolithic H5AD/SnapATAC2 collection; output matrix allocated by Python API",
        "python_imports_ns": None,
        "measurement_mode": "instrumented_diagnostic" if diagnostic else "uninstrumented_timing",
        "diagnostic": diag,
    }


def validate_existing_10k_controls(root: Path) -> dict[str, object]:
    """Check Python support and formula goldens against existing 10k fixtures."""
    if not (root / "inventory.json").is_file():
        raise FileNotFoundError(f"existing fixture inventory not found: {root / 'inventory.json'}")
    checks = {}
    directory = root / "10000" if (root / "10000").is_dir() else root
    for case in ("anndata-10k-control", "dataset-10k-control"):
        modules = _imports_for(case)
        is_dataset = case == "dataset-10k-control"
        path = directory / ("collection.h5ads" if is_dataset else "monolithic.h5ad")
        data = modules["snap"].read_dataset(path, mode="r") if is_dataset else modules["ad"].read_h5ad(path, backed="r")
        rows = _legacy_spread_positions(10_000)
        cols = _legacy_spread_positions(20_000)
        result = _assert_csr(_select_csr(data.X, modules, rows, cols), modules,
                             _legacy_10k_golden(rows, cols))
        _close(data, "dataset" if is_dataset else "anndata")
        checks[case] = result
    return checks


def _matrix_for_case(case: str, root: Path, modules: dict[str, object]):
    path = root / FIXTURES[CASE_FAMILY[case]]
    family = CASE_FAMILY[case]
    if family == "anndata":
        data = modules["ad"].read_h5ad(path, backed="r")
        return data, data.X, "backed H5AD CSRDataset; selected indexing reads requested rows/columns"
    if family == "mudata":
        data = modules["mudata"].read_h5mu(path, backed="r")
        return data, data.mod["rna"].X, "one requested rna modality; backed H5MU"
    if family == "treedata":
        data = modules["treedata"].read_h5td(path, backed="r")
        return data, data.X, "H5TD API may eagerly decode tree metadata during open"
    if family == "dataset":
        data = modules["snap"].read_dataset(path, mode="r")
        return data, data.X, "SnapATAC2 AnnDataSet indexed X API"
    if family == "spatialdata":
        data = modules["spatialdata"].read_zarr(path, selection=("tables", "labels"))
        return data, data.tables["table"].X, (
            "read_zarr selects tables and its referenced labels/cells region; "
            "upstream selection is by element group"
        )
    raise RuntimeError(f"no matrix reader for {case}")


def _close(data, family: str) -> None:
    try:
        if family == "anndata" and getattr(data, "file", None) is not None:
            data.file.close()
        elif hasattr(data, "close"):
            data.close()
    except Exception:
        # Close support varies across the reference APIs; case correctness is
        # already established and process exit releases remaining handles.
        pass


def _case_matrix(case: str, root: Path, modules: dict[str, object], diagnostic: bool) -> dict[str, object]:
    family = CASE_FAMILY[case]
    path = root / FIXTURES[family]
    observer = Observer(diagnostic)
    observer.begin()
    start = time.perf_counter_ns()
    data, matrix, api_boundary = _matrix_for_case(case, root, modules)
    opened = time.perf_counter_ns()
    selected = _select_csr(matrix, modules)
    finished = time.perf_counter_ns()
    diag = observer.end()
    result = _assert_csr(selected, modules)
    _close(data, family)
    return {
        "case": case,
        "fixture": str(path.resolve()),
        "selector": "rows_and_columns_0_4_252",
        "shape": result["shape"],
        "nnz": result["nnz"],
        "value_dtype": result["value_dtype"],
        "canonical_index_dtype": result["canonical_index_dtype"],
        "canonical_offset_dtype": result["canonical_offset_dtype"],
        "sha256": result["sha256"],
        "golden": result["golden"],
        "hash_source": result["hash_source"],
        "api_boundary": api_boundary,
        "open_ns": opened - start,
        "prepare_and_allocate_ns": 0,
        "operation_ns": finished - opened,
        "end_to_end_ns": finished - start,
        "operation_comparable_to_cpp_caller_buffer": False,
        "comparison_basis": "same independently checked CSR output; compare end-to-end only",
        "python_imports_ns": time.perf_counter_ns() - SCRIPT_START_NS,
        "measurement_mode": "instrumented_diagnostic" if diagnostic else "uninstrumented_timing",
        "diagnostic": diag,
    }


def _hash_numeric(arrays) -> str:
    return digest(b"".join(bytes(x) for x in arrays))


def _case_mudata_map(root: Path, modules: dict[str, object], diagnostic: bool) -> dict[str, object]:
    np = modules["np"]
    path = root / FIXTURES["mudata"]
    rows = np.asarray(ROWS, dtype=np.int64)
    cols = np.asarray(COLS, dtype=np.int64)
    observer = Observer(diagnostic)
    observer.begin()
    start = time.perf_counter_ns()
    data = modules["mudata"].read_h5mu(path, backed="r")
    opened = time.perf_counter_ns()
    selected = data[rows, cols]
    obs_map = np.asarray(selected.obsmap["rna"], dtype=np.uint64).reshape(-1)
    var_map = np.asarray(selected.varmap["rna"], dtype=np.uint64).reshape(-1)
    finished = time.perf_counter_ns()
    diag = observer.end()
    expected = np.arange(1, 65, dtype=np.uint64)
    if not np.array_equal(obs_map, expected) or not np.array_equal(var_map, expected):
        raise RuntimeError("MuData selected rna map differs from independently expected projected positions")
    actual_sha = _hash_numeric((obs_map.astype("<u8").tobytes(), var_map.astype("<u8").tobytes()))
    expected_sha = _hash_numeric((expected.astype("<u8").tobytes(), expected.astype("<u8").tobytes()))
    if actual_sha != expected_sha:
        raise RuntimeError("MuData selected map hash mismatch")
    _close(data, "mudata")
    return {
        "case": "mudata-map",
        "fixture": str(path.resolve()),
        "selector": "selected global rows/columns 0,4,...,252 for modality rna",
        "modality": "rna",
        "obs_map": obs_map.tolist(),
        "var_map": var_map.tolist(),
        "map_value_semantics": "one-based selected modality positions; zero means absent",
        "sha256": actual_sha,
        "golden": "pass",
        "open_ns": opened - start,
        "prepare_and_allocate_ns": 0,
        "operation_ns": finished - opened,
        "end_to_end_ns": finished - start,
        "api_boundary": "MuData reads global maps and constructs an in-memory selected MuData view",
        "operation_comparable_to_cpp_lazy_backed_map": False,
        "comparison_basis": "same projected map values; Python read_h5mu may eagerly load map payloads",
        "python_imports_ns": time.perf_counter_ns() - SCRIPT_START_NS,
        "measurement_mode": "instrumented_diagnostic" if diagnostic else "uninstrumented_timing",
        "diagnostic": diag,
    }


def _tree_digest(names, ranks, sources, targets, lengths) -> str:
    import numpy as np

    name_bytes = b"\0".join(value.encode("utf-8") for value in names) + b"\0"
    payload = (
        name_bytes
        + np.asarray(ranks, dtype="<i8").tobytes()
        + np.asarray(sources, dtype="<u8").tobytes()
        + np.asarray(targets, dtype="<u8").tobytes()
        + np.asarray(lengths, dtype="<f8").tobytes()
    )
    return digest(payload)


def _tree_golden_sha256(selected_rows) -> str:
    nodes = ["root", *[f"cell-{i:03d}" for i in selected_rows]]
    ranks = [-1, *[int(i) for i in selected_rows]]
    sources = [0] * len(selected_rows)
    targets = list(range(1, len(selected_rows) + 1))
    lengths = [1.0 + int(i) / 256.0 for i in selected_rows]
    return _tree_digest(nodes, ranks, sources, targets, lengths)


def _points_golden_sha256() -> str:
    import numpy as np

    rows = np.arange(16, 24, dtype=np.int64)
    coords = np.column_stack((rows.astype(np.float64) + 0.25, 2.0 * rows + 0.5)).astype("<f8")
    ids = (1000 + rows).astype("<i8")
    return digest(coords.tobytes(order="C") + ids.tobytes(order="C"))


def _tree_projection_signature(graph) -> dict[str, object]:
    def node_order(name: str) -> tuple[int, int | str]:
        if name == "root":
            return (0, "")
        if name.startswith("cell-"):
            return (1, int(name.removeprefix("cell-")))
        return (2, name)

    nodes = sorted((str(name) for name in graph.nodes), key=node_order)
    index = {name: i for i, name in enumerate(nodes)}
    edges = sorted(
        graph.edges(data=True),
        key=lambda edge: (node_order(str(edge[0])), node_order(str(edge[1]))),
    )
    # Build rank values in the canonical node order, not NetworkX iteration order.
    ranks = [int(graph.nodes[name]["rank"]) for name in nodes]
    sources = [index[str(u)] for u, _v, _attrs in edges]
    targets = [index[str(v)] for _u, v, _attrs in edges]
    lengths = [float(attrs["length"]) for _u, _v, attrs in edges]
    return {
        "nodes": nodes,
        "ranks": ranks,
        "edges": [[str(u), str(v)] for u, v, _attrs in edges],
        "edge_lengths": lengths,
        "sha256": _tree_digest(nodes, ranks, sources, targets, lengths),
    }


def _case_treedata_tree(root: Path, modules: dict[str, object], diagnostic: bool) -> dict[str, object]:
    path = root / FIXTURES["treedata"]
    observer = Observer(diagnostic)
    observer.begin()
    start = time.perf_counter_ns()
    data = modules["treedata"].read_h5td(path, backed="r")
    opened = time.perf_counter_ns()
    names = sorted(str(name) for name in data.obst.keys())
    names_done = time.perf_counter_ns()
    lineage = data.obst["lineage"]
    keyed_done = time.perf_counter_ns()
    # TreeData slicing is a view; to_memory() materializes the selected AnnData
    # payload and retains/project all trees, matching C++ read(selection).
    selected = data[list(ROWS), :].to_memory(copy=True)
    selected_lineage = selected.obst["lineage"]
    finished = time.perf_counter_ns()
    diag = observer.end()
    full = _tree_projection_signature(lineage)
    projected = _tree_projection_signature(selected_lineage)
    expected_names = ["lineage", "unused"]
    expected_nodes = ["root", *[f"cell-{i:03d}" for i in ROWS]]
    expected_ranks = [-1, *ROWS]
    expected_edges = [["root", f"cell-{i:03d}"] for i in ROWS]
    expected_lengths = [1.0 + i / 256.0 for i in ROWS]
    if names != expected_names:
        raise RuntimeError(f"TreeData names {names!r} != {expected_names!r}")
    if len(full["nodes"]) != 257 or len(full["edges"]) != 256:
        raise RuntimeError("TreeData full lineage topology dimensions differ from formula")
    if projected["nodes"] != expected_nodes or projected["ranks"] != expected_ranks:
        raise RuntimeError("TreeData selected lineage node IDs/ranks differ from formula")
    if projected["edges"] != expected_edges or projected["edge_lengths"] != expected_lengths:
        raise RuntimeError("TreeData selected lineage topology/edge attributes differ from formula")
    _close(data, "treedata")
    return {
        "case": "treedata-tree",
        "fixture": str(path.resolve()),
        "selector": "obs rows 0,4,...,252; request named tree lineage",
        "tree_names": names,
        "names_ns": names_done - opened,
        "named_tree_ns": keyed_done - names_done,
        "selected_read_ns": finished - keyed_done,
        "full_lineage": full,
        "selected_lineage": projected,
        "golden": "pass",
        "open_ns": opened - start,
        "end_to_end_ns": finished - start,
        "api_boundary": "pinned treedata read_h5td may decode the scalar tree collection eagerly; selected slicing projects the in-memory TreeData",
        "operation_comparable_to_cpp_keyed_backed_read": False,
        "comparison_basis": "same named tree and selected topology/attributes; open includes eager Python parsing",
        "python_imports_ns": time.perf_counter_ns() - SCRIPT_START_NS,
        "measurement_mode": "instrumented_diagnostic" if diagnostic else "uninstrumented_timing",
        "diagnostic": diag,
    }


def _case_spatial_raster(root: Path, modules: dict[str, object], diagnostic: bool) -> dict[str, object]:
    import numpy as np

    path = root / FIXTURES["spatialdata"]
    observer = Observer(diagnostic)
    observer.begin()
    start = time.perf_counter_ns()
    data = modules["spatialdata"].read_zarr(path, selection=("images",))
    opened = time.perf_counter_ns()
    image = data.images["image"]
    if hasattr(image, "children"):
        child_names = list(image.children)
        level = "scale0" if "scale0" in child_names else child_names[0]
        image = image[level]["image"]
    selected = image.isel(c=slice(0, 1), y=slice(16, 80, 2), x=slice(32, 96, 2)).data
    if hasattr(selected, "compute"):
        selected = selected.compute()
    selected = np.asarray(selected, dtype="<u2")
    finished = time.perf_counter_ns()
    diag = observer.end()
    golden = np.asarray(
        [[256 * row + col for col in range(32, 96, 2)] for row in range(16, 80, 2)],
        dtype="<u2",
    )[None, :, :]
    actual_sha = digest(selected.tobytes(order="C"))
    expected_sha = digest(golden.tobytes(order="C"))
    if selected.shape != (1, 32, 32) or not np.array_equal(selected, golden) or actual_sha != expected_sha:
        raise RuntimeError("SpatialData raster window differs from independent pixel formula")
    return {
        "case": "spatialdata-raster",
        "fixture": str(path.resolve()),
        "selector": "image level 0; c[0:1], y[16:80:2], x[32:96:2]",
        "shape": list(selected.shape),
        "dtype": "uint16",
        "sha256": actual_sha,
        "golden": "pass",
        "open_ns": opened - start,
        "prepare_and_allocate_ns": 0,
        "operation_ns": finished - opened,
        "end_to_end_ns": finished - start,
        "api_boundary": "SpatialData read_zarr selects only images; xarray/Dask computes the requested raster window",
        "operation_comparable_to_cpp_caller_buffer": False,
        "comparison_basis": "same raster window; Python creates a returned NumPy array, C++ writes caller-owned buffer",
        "python_imports_ns": time.perf_counter_ns() - SCRIPT_START_NS,
        "measurement_mode": "instrumented_diagnostic" if diagnostic else "uninstrumented_timing",
        "diagnostic": diag,
    }


def _case_spatial_points(root: Path, modules: dict[str, object], diagnostic: bool) -> dict[str, object]:
    import numpy as np

    path = root / FIXTURES["spatialdata"]
    observer = Observer(diagnostic)
    observer.begin()
    start = time.perf_counter_ns()
    data = modules["spatialdata"].read_zarr(path, selection=("points",))
    opened = time.perf_counter_ns()
    frame = data.points["points"]
    # Dask GeoDataFrame does not expose a general row-range read contract.
    # Its public API computes the selected partition(s) only when .compute()
    # is called; if positional slicing is unsupported, preserve the full eager
    # work and label that boundary rather than substituting direct Parquet IO.
    try:
        selected = frame.iloc[16:24].compute()
        api_boundary = "Dask GeoDataFrame iloc row slice then compute"
    except (AttributeError, NotImplementedError, TypeError, ValueError):
        selected = frame.compute().iloc[16:24]
        api_boundary = "SpatialData points require full Dask frame compute before pandas row slice"
    coords = np.column_stack(
        [selected["x"].to_numpy(dtype="<f8"), selected["y"].to_numpy(dtype="<f8")]
    )
    ids = selected["spot_id"].to_numpy(dtype="<i8")
    finished = time.perf_counter_ns()
    diag = observer.end()
    rows = np.arange(16, 24, dtype=np.int64)
    want_coords = np.column_stack((rows.astype(np.float64) + 0.25, 2.0 * rows + 0.5)).astype("<f8")
    want_ids = (1000 + rows).astype("<i8")
    if not np.array_equal(coords, want_coords) or not np.array_equal(ids, want_ids):
        raise RuntimeError("SpatialData selected point batch differs from independent formula")
    actual_sha = digest(coords.tobytes(order="C") + ids.tobytes(order="C"))
    expected_sha = digest(want_coords.tobytes(order="C") + want_ids.tobytes(order="C"))
    if actual_sha != expected_sha:
        raise RuntimeError("SpatialData point batch SHA256 differs from independent formula")
    return {
        "case": "spatialdata-points",
        "fixture": str(path.resolve()),
        "selector": {"first": 16, "count": 8},
        "columns": ["x", "y", "spot_id"],
        "coordinates": coords.tolist(),
        "spot_id": ids.tolist(),
        "sha256": actual_sha,
        "golden": "pass",
        "open_ns": opened - start,
        "prepare_and_allocate_ns": 0,
        "operation_ns": finished - opened,
        "end_to_end_ns": finished - start,
        "api_boundary": api_boundary,
        "operation_comparable_to_cpp_bounded_parquet_read": api_boundary.startswith("Dask GeoDataFrame iloc"),
        "comparison_basis": "same exact point rows/values; Python API row materialization is recorded above",
        "python_imports_ns": time.perf_counter_ns() - SCRIPT_START_NS,
        "measurement_mode": "instrumented_diagnostic" if diagnostic else "uninstrumented_timing",
        "diagnostic": diag,
    }


def validate_fixture_set(root: Path) -> dict[str, object]:
    """Read fixtures and check the formulas without collecting timings."""
    _load_manifest(root)
    checks: dict[str, object] = {}

    for case in ("anndata", "mudata", "treedata", "dataset", "spatialdata"):
        modules = _imports_for(case)
        data, matrix, _boundary = _matrix_for_case(case, root, modules)
        checks[case] = _assert_csr(_select_csr(matrix, modules), modules)
        _close(data, CASE_FAMILY[case])

    modules = _imports_for("mudata-map")
    np = modules["np"]
    maps = modules["mudata"].read_h5mu(root / FIXTURES["mudata"], backed="r")
    selected_maps = maps[np.asarray(ROWS), np.asarray(COLS)]
    map_obs = np.asarray(selected_maps.obsmap["rna"], dtype=np.uint64).reshape(-1)
    map_var = np.asarray(selected_maps.varmap["rna"], dtype=np.uint64).reshape(-1)
    want_map = np.arange(1, 65, dtype=np.uint64)
    if not np.array_equal(map_obs, want_map) or not np.array_equal(map_var, want_map):
        raise RuntimeError("MuData selected map fixture readback differs from expected one-based positions")
    checks["mudata-map"] = {"golden": "pass", "obs_count": len(map_obs), "var_count": len(map_var)}
    _close(maps, "mudata")

    modules = _imports_for("treedata-tree")
    tree_data = modules["treedata"].read_h5td(root / FIXTURES["treedata"], backed="r")
    names = sorted(str(name) for name in tree_data.obst.keys())
    full = _tree_projection_signature(tree_data.obst["lineage"])
    selected_data = tree_data[list(ROWS), :].to_memory(copy=True)
    selected = _tree_projection_signature(selected_data.obst["lineage"])
    expected_nodes = ["root", *[f"cell-{i:03d}" for i in ROWS]]
    expected_edges = [["root", f"cell-{i:03d}"] for i in ROWS]
    if names != ["lineage", "unused"] or len(full["nodes"]) != 257 or len(full["edges"]) != 256:
        raise RuntimeError("TreeData full fixture names/topology differ from expected format")
    if selected["nodes"] != expected_nodes or len(selected["edges"]) != 64:
        raise RuntimeError("TreeData selected fixture topology differs from expected formula")
    if selected["sha256"] != _tree_golden_sha256(ROWS):
        raise RuntimeError("TreeData selected fixture hash differs from independent formula")
    checks["treedata-tree"] = {"golden": "pass", "tree_names": names,
                               "full_nodes": len(full["nodes"]), "selected_nodes": len(selected["nodes"]),
                               "selected_edges": len(selected["edges"]),
                               "selected_sha256": selected["sha256"]}
    _close(tree_data, "treedata")

    modules = _imports_for("spatialdata-raster")
    spatial = modules["spatialdata"].read_zarr(root / FIXTURES["spatialdata"], selection=("images",))
    raster = spatial.images["image"]
    if hasattr(raster, "children"):
        children = list(raster.children)
        level = "scale0" if "scale0" in children else children[0]
        raster = raster[level]["image"]
    pixels = raster.isel(c=slice(0, 1), y=slice(16, 80, 2), x=slice(32, 96, 2)).data
    if hasattr(pixels, "compute"):
        pixels = pixels.compute()
    pixels = np.asarray(pixels, dtype="<u2")
    want_pixels = np.asarray(
        [[256 * y + x for x in range(32, 96, 2)] for y in range(16, 80, 2)],
        dtype="<u2",
    )[None, :, :]
    if pixels.shape != (1, 32, 32) or not np.array_equal(pixels, want_pixels):
        raise RuntimeError("SpatialData raster fixture readback differs from pixel formula")
    checks["spatialdata-raster"] = {"golden": "pass", "shape": list(pixels.shape),
                                    "sha256": digest(pixels.tobytes(order="C"))}

    modules = _imports_for("spatialdata-points")
    point_data = modules["spatialdata"].read_zarr(root / FIXTURES["spatialdata"], selection=("points",))
    frame = point_data.points["points"]
    try:
        points = frame.iloc[16:24].compute()
    except (AttributeError, NotImplementedError, TypeError, ValueError):
        points = frame.compute().iloc[16:24]
    rows = np.arange(16, 24, dtype=np.int64)
    got_xy = np.column_stack((points["x"].to_numpy(dtype="<f8"), points["y"].to_numpy(dtype="<f8")))
    got_ids = points["spot_id"].to_numpy(dtype="<i8")
    want_xy = np.column_stack((rows.astype(np.float64) + 0.25, 2.0 * rows + 0.5)).astype("<f8")
    want_ids = (1000 + rows).astype("<i8")
    if not np.array_equal(got_xy, want_xy) or not np.array_equal(got_ids, want_ids):
        raise RuntimeError("SpatialData points fixture readback differs from row formula")
    checks["spatialdata-points"] = {"golden": "pass", "count": len(got_ids),
                                    "sha256": digest(got_xy.tobytes(order="C") + got_ids.tobytes(order="C"))}

    return checks


def run_case(case: str, root: Path, diagnostic: bool) -> dict[str, object]:
    if case not in CASE_FAMILY:
        raise ValueError(f"unknown case {case!r}; choose {', '.join(CASE_FAMILY)}")
    manifest = _load_manifest(root)
    modules = _imports_for(case)
    expected_versions = manifest.get("package_versions", {})
    package_name = {"dataset": "snapatac2"}.get(CASE_FAMILY[case], CASE_FAMILY[case])
    required = {"anndata"} | {package_name}
    for name in required:
        expected = PACKAGES.get(name)
        actual = expected_versions.get(name)
        if actual != expected:
            raise RuntimeError(f"fixture pin mismatch for {name}: expected {expected}, inventory has {actual}")
        if importlib.metadata.version(name) != expected:
            raise RuntimeError(f"expected {name}=={expected}, found {importlib.metadata.version(name)}")
    import_ns = time.perf_counter_ns() - SCRIPT_START_NS
    if case == "mudata-map":
        result = _case_mudata_map(root, modules, diagnostic)
    elif case == "treedata-tree":
        result = _case_treedata_tree(root, modules, diagnostic)
    elif case == "spatialdata-raster":
        result = _case_spatial_raster(root, modules, diagnostic)
    elif case == "spatialdata-points":
        result = _case_spatial_points(root, modules, diagnostic)
    else:
        result = _case_matrix(case, root, modules, diagnostic)
    result["python_imports_ns"] = import_ns
    result["fixture_inventory"] = str((root / "inventory.json").resolve())
    result["fixture_inventory_sha256"] = digest((root / "inventory.json").read_bytes())
    result["package_versions"] = package_versions()
    result["timing_scope"] = "fixture opened through requested output; Python imports outside phase timer"
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare", help="create a new fixture directory and full file inventory")
    prep.add_argument("output", type=Path)
    validate = commands.add_parser("validate", help="read fixture data and check independent formulas without timings")
    validate.add_argument("fixtures", type=Path)
    case = commands.add_parser("case", help="run one family case in this process")
    case.add_argument("name", choices=tuple(CASE_FAMILY))
    case.add_argument("fixtures", type=Path)
    case.add_argument("--diagnostic", action="store_true", help="enable existing v1 LD_PRELOAD diagnostic counters")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.output)
    elif args.command == "validate":
        fixtures = args.fixtures.resolve()
        receipt_path = fixtures / "validation.json"
        if receipt_path.exists():
            raise FileExistsError(f"refusing to overwrite fixture validation receipt: {receipt_path}")
        checks = validate_fixture_set(fixtures)
        driver_sha = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        inventory_sha = hashlib.sha256((fixtures / "inventory.json").read_bytes()).hexdigest()
        receipt = {
            "schema": "anndatapp.format_family_fixture_validation.v1",
            "status": "fixture_readback_passed",
            "created_at_utc": datetime.now(timezone.utc).isoformat(),
            "fixture_root": str(fixtures),
            "fixture_inventory": "inventory.json",
            "fixture_inventory_sha256": inventory_sha,
            "driver": str(Path(__file__).resolve()),
            "driver_sha256": driver_sha,
            "package_versions": package_versions(),
            "checks": checks,
            "timing_runs": 0,
        }
        with receipt_path.open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
            stream.write("\n")
        print(json.dumps({"status": receipt["status"], "receipt": str(receipt_path),
                          "fixture_inventory_sha256": inventory_sha, "checks": checks},
                         sort_keys=True, separators=(",", ":")))
    else:
        run_case(args.name, args.fixtures.resolve(), args.diagnostic)


if __name__ == "__main__":
    warnings.filterwarnings("ignore", category=FutureWarning)
    main()
