#!/usr/bin/env python3
"""Create and check a small SnapATAC2 AnnDataSet interoperability fixture.

SnapATAC2 2.10.0's pinned anndata-rs implementation stores
uns['AnnDataSet'] as a DataFrame with keys and file_path columns, requires
identical ordered var_names across components, and resolves relative component
paths from the .h5ads file's parent:
https://github.com/scverse/anndata-rs/blob/cf0b312f2834884bd708595a63f12f34f1905b7b/anndata/src/anndata/dataset.rs
"""
from __future__ import annotations

import argparse
from importlib.metadata import version
import os
from pathlib import Path
import shutil
import tempfile

import anndata as ad
import numpy as np
import pandas as pd
import scipy.sparse as sp
import snapatac2 as snap


COMPONENT_KEYS = ["first", "second"]
COMPONENT_FILES = ["first.h5ad", "second.h5ad"]
VAR_NAMES = ["peak-z", "peak-a", "gene-m"]
OBS_NAMES = ["cell-duplicate", "cell-first", "cell-duplicate", "cell-second"]
SAMPLES = ["first", "first", "second", "second"]
CONDITIONS = ["control", "treated", "treated", "control"]
EXPECTED_X = np.array(
    [
        [1.0, 0.0, 2.0],
        [0.0, 3.0, 0.0],
        [0.0, 4.0, 5.0],
        [6.0, 0.0, 0.0],
    ],
    dtype=np.float32,
)


def require_snap_version() -> None:
    installed = version("snapatac2")
    if installed != "2.10.0":
        raise RuntimeError(f"expected SnapATAC2 2.10.0, found {installed}")


def make_component(which: int) -> ad.AnnData:
    if which == 0:
        obs_names = ["cell-duplicate", "cell-first"]
        conditions = ["control", "treated"]
        batches = np.array([10, 11], dtype=np.int32)
        matrix = np.array([[1, 0, 2], [0, 3, 0]], dtype=np.float32)
    else:
        obs_names = ["cell-duplicate", "cell-second"]
        conditions = ["treated", "control"]
        batches = np.array([20, 21], dtype=np.int32)
        matrix = np.array([[0, 4, 5], [6, 0, 0]], dtype=np.float32)

    obs = pd.DataFrame(
        {"condition": conditions, "batch": batches},
        index=pd.Index(obs_names, name="cell_id"),
    )
    var = pd.DataFrame(
        {"feature_type": ["peak", "peak", "gene"]},
        index=pd.Index(VAR_NAMES, name="feature_id"),
    )
    result = ad.AnnData(X=sp.csr_matrix(matrix), obs=obs, var=var)
    result.uns["fixture_marker"] = "snap-dataset-fixture-v1"
    return result


def _as_list(values) -> list:
    if hasattr(values, "tolist"):
        return values.tolist()
    return list(values)


def _manifest(dataset) -> tuple[list[str], list[str]]:
    frame = dataset.uns["AnnDataSet"]
    if not hasattr(frame, "columns"):
        if hasattr(frame, "to_pandas"):
            frame = frame.to_pandas()
        else:
            frame = pd.DataFrame(frame)
    columns = list(frame.columns)
    assert columns[:2] == ["keys", "file_path"], f"unexpected manifest columns: {columns}"
    return (
        [str(value) for value in _as_list(frame["keys"])],
        [str(value) for value in _as_list(frame["file_path"])],
    )


def _component_paths(
    dataset_path: Path, paths: list[str], *, require_relative: bool
) -> list[Path]:
    result = []
    for raw_path in paths:
        stored_path = Path(raw_path)
        if require_relative:
            assert not stored_path.is_absolute(), (
                f"expected relative component path, got {raw_path}"
            )
        path = stored_path if stored_path.is_absolute() else dataset_path.parent / stored_path
        path = path.resolve()
        assert path.is_file(), f"missing component file {path} referenced by {dataset_path}"
        result.append(path)
    return result


def _dense(matrix) -> np.ndarray:
    if sp.issparse(matrix):
        return matrix.toarray()
    if hasattr(matrix, "to_memory"):
        matrix = matrix.to_memory()
        if sp.issparse(matrix):
            return matrix.toarray()
    if hasattr(matrix, "toarray"):
        return matrix.toarray()
    return np.asarray(matrix)


def _dense_stacked(dataset) -> np.ndarray:
    pieces = []
    next_row = 0
    for matrix, start, end in dataset.X.chunked(max(1, int(dataset.n_obs))):
        start = int(start)
        end = int(end)
        assert start == next_row, f"noncontiguous matrix chunk begins at row {start}"
        piece = _dense(matrix)
        assert piece.shape == (end - start, int(dataset.n_vars))
        pieces.append(piece)
        next_row = end
    assert next_row == int(dataset.n_obs), (
        f"read {next_row} matrix rows, expected {dataset.n_obs}"
    )
    return np.concatenate(pieces, axis=0)


def assert_dataset(
    dataset, dataset_path: Path, *, require_relative: bool = True
) -> list[Path]:
    assert tuple(dataset.shape) == (4, 3), f"unexpected shape: {dataset.shape}"
    assert int(dataset.n_obs) == 4
    assert int(dataset.n_vars) == 3
    assert _as_list(dataset.obs_names) == OBS_NAMES
    assert _as_list(dataset.var_names) == VAR_NAMES

    obs = dataset.obs
    assert _as_list(obs["sample"]) == SAMPLES

    np.testing.assert_array_equal(_dense_stacked(dataset), EXPECTED_X)

    keys, paths = _manifest(dataset)
    assert keys == COMPONENT_KEYS, f"unexpected component order: {keys}"
    component_paths = _component_paths(
        dataset_path, paths, require_relative=require_relative
    )
    for expected_name, component_path in zip(
        COMPONENT_FILES, component_paths, strict=True
    ):
        assert component_path.name == expected_name, (
            f"component {expected_name} points to {component_path.name}"
        )

    expected_component_conditions = [["control", "treated"], ["treated", "control"]]
    expected_component_batches = [[10, 11], [20, 21]]
    for index, component_path in enumerate(component_paths):
        component = ad.read_h5ad(component_path)
        assert component.shape == (2, 3)
        assert sp.isspmatrix_csr(component.X)
        assert component.obs_names.tolist() == OBS_NAMES[index * 2 : index * 2 + 2]
        assert component.var_names.tolist() == VAR_NAMES
        assert component.obs["condition"].tolist() == expected_component_conditions[index]
        assert component.obs["batch"].tolist() == expected_component_batches[index]
        assert component.var["feature_type"].tolist() == ["peak", "peak", "gene"]
        assert component.uns["fixture_marker"] == "snap-dataset-fixture-v1"

    return component_paths


def create(directory: Path) -> Path:
    require_snap_version()
    root = directory.resolve()
    root.mkdir(parents=True, exist_ok=True)
    expected_files = [root / name for name in (*COMPONENT_FILES, "dataset.h5ads")]
    existing = [str(path) for path in expected_files if path.exists()]
    if existing:
        raise FileExistsError("refusing to overwrite fixture files: " + ", ".join(existing))

    for index, name in enumerate(COMPONENT_FILES):
        make_component(index).write_h5ad(root / name)

    # Relative names ensure manifest paths resolve from dataset.h5ads.
    previous_cwd = Path.cwd()
    try:
        os.chdir(root)
        dataset = snap.AnnDataSet(
            [
                (key, Path(name))
                for key, name in zip(COMPONENT_KEYS, COMPONENT_FILES, strict=True)
            ],
            filename=Path("dataset.h5ads"),
            add_key="sample",
            use_absolute_path=False,
            backend="hdf5",
        )
        dataset.close()
    finally:
        os.chdir(previous_cwd)

    check(root / "dataset.h5ads", exercise_relocation=False)
    return root / "dataset.h5ads"


def _check_relocation(dataset_path: Path, component_paths: list[Path]) -> None:
    with tempfile.TemporaryDirectory(prefix="anndatapp-dataset-relocation-") as temp_name:
        temp = Path(temp_name)
        relocated_dir = temp / "components"
        relocated_dir.mkdir()
        for source in component_paths:
            shutil.copy2(source, relocated_dir / source.name)

        # The reference API may update the manifest while opening. Use a
        # temporary copy so the checked fixture remains untouched.
        relocated_dataset_path = temp / "dataset.h5ads"
        shutil.copy2(dataset_path, relocated_dataset_path)
        dataset = snap.read_dataset(
            relocated_dataset_path,
            adata_files_update=relocated_dir,
            mode="r+",
        )
        try:
            updated_paths = assert_dataset(
                dataset, relocated_dataset_path, require_relative=False
            )
            assert updated_paths == [
                (relocated_dir / name).resolve() for name in COMPONENT_FILES
            ]
        finally:
            dataset.close()


def check(path: Path, *, exercise_relocation: bool = True) -> None:
    require_snap_version()
    dataset_path = path.resolve()
    dataset = snap.read_dataset(dataset_path, mode="r")
    try:
        component_paths = assert_dataset(dataset, dataset_path)
    finally:
        dataset.close()

    if exercise_relocation:
        _check_relocation(dataset_path, component_paths)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create_parser = commands.add_parser("create", help="create the independent fixture")
    create_parser.add_argument("directory", type=Path)
    check_parser = commands.add_parser("check", help="check a .h5ads dataset with SnapATAC2")
    check_parser.add_argument("path", type=Path)
    args = parser.parse_args()

    if args.command == "create":
        print(create(args.directory))
    else:
        check(args.path)
        print(f"SnapATAC2 2.10.0 accepted {args.path.resolve()}")


if __name__ == "__main__":
    main()
