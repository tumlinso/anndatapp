#!/usr/bin/env python3
"""Independent full SpatialData current/legacy -> C++ -> upstream qualification."""
from __future__ import annotations

import argparse
import importlib.metadata
import os
from pathlib import Path
import subprocess
import sys
import uuid

if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") == "1":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from python_storage import install_restricted_runner_wakeup  # noqa: F401

import geopandas as gpd
import numpy as np
import pandas as pd
import spatialdata as sd
import zarr
from spatialdata.transformations import get_transformation


def frame_equal(a, b):
    assert list(a.columns) == list(b.columns), (list(a.columns), list(b.columns))
    np.testing.assert_array_equal(a.index.to_numpy(), b.index.to_numpy())
    if pd.api.types.is_numeric_dtype(a.index.dtype):
        assert a.index.dtype == b.index.dtype
    for name in a:
        x, y = a[name], b[name]
        if isinstance(x.dtype, pd.CategoricalDtype):
            assert isinstance(y.dtype, pd.CategoricalDtype)
            assert x.cat.ordered == y.cat.ordered
            np.testing.assert_array_equal(x.cat.categories.to_numpy(), y.cat.categories.to_numpy())
            np.testing.assert_array_equal(x.cat.codes.to_numpy(), y.cat.codes.to_numpy())
        elif pd.api.types.is_numeric_dtype(x.dtype):
            assert x.dtype == y.dtype, (name, x.dtype, y.dtype)
            np.testing.assert_array_equal(x.to_numpy(), y.to_numpy())
        else:
            pd.testing.assert_series_equal(x, y, check_dtype=False, check_index_type=False)


def levels(raster):
    if hasattr(raster, "children"):
        return [raster[name]["image"] for name in raster.children]
    return [raster]


def transforms_equal(a, b, axes):
    x = get_transformation(a, get_all=True)
    y = get_transformation(b, get_all=True)
    assert set(x) == set(y), (set(x), set(y))
    for name in x:
        np.testing.assert_allclose(
            x[name].to_affine_matrix(input_axes=axes, output_axes=axes),
            y[name].to_affine_matrix(input_axes=axes, output_axes=axes),
            rtol=1e-13, atol=1e-13,
        )


def compare(a, b):
    for kind in ("images", "labels", "points", "shapes", "tables"):
        assert set(getattr(a, kind)) == set(getattr(b, kind)), kind
    for kind in ("images", "labels"):
        for name, raster in getattr(a, kind).items():
            other = getattr(b, kind)[name]
            x, y = levels(raster), levels(other)
            assert len(x) == len(y)
            for p, q in zip(x, y, strict=True):
                assert tuple(p.dims) == tuple(q.dims)
                assert p.dtype == q.dtype
                np.testing.assert_array_equal(p.values, q.values)
                if "c" in p.coords:
                    np.testing.assert_array_equal(p.coords["c"].values, q.coords["c"].values)
            transforms_equal(raster, other, tuple(x[0].dims))
    for name, points in a.points.items():
        other = b.points[name]
        frame_equal(points.compute(), other.compute())
        axes = tuple(k for k in ("x", "y", "z") if k in points.columns)
        transforms_equal(points, other, axes)
    for name, shapes in a.shapes.items():
        other = b.shapes[name]
        frame_equal(shapes.drop(columns=shapes.geometry.name), other.drop(columns=other.geometry.name))
        assert list(shapes.geom_type) == list(other.geom_type)
        for p, q in zip(shapes.geometry, other.geometry, strict=True):
            assert p.equals_exact(q, 0), (p.wkt, q.wkt)
        transforms_equal(shapes, other, ("x", "y"))
    for name, table in a.tables.items():
        other = b.tables[name]
        assert table.shape == other.shape
        if table.X is None:
            assert other.X is None
        else:
            assert table.X.dtype == other.X.dtype
            np.testing.assert_array_equal(table.X, other.X)
        frame_equal(table.obs, other.obs)
        frame_equal(table.var, other.var)
        x = table.uns.get("spatialdata_attrs")
        y = other.uns.get("spatialdata_attrs")
        assert (x is None) == (y is None)
        if x is not None:
            assert x["region_key"] == y["region_key"]
            assert x["instance_key"] == y["instance_key"]
            np.testing.assert_array_equal(np.asarray(x["region"]), np.asarray(y["region"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--legacy02", type=Path)
    parser.add_argument("--legacy03", type=Path)
    args = parser.parse_args()
    if importlib.metadata.version("spatialdata") != "0.9.0":
        raise RuntimeError("full qualification requires spatialdata==0.9.0")
    args.output.mkdir(parents=True, exist_ok=True)
    run = args.output / ("run-" + uuid.uuid4().hex)
    run.mkdir()
    source = Path(__file__).resolve().parents[1]
    native = run / "native.zarr"
    native_copy = run / "native-cpp.zarr"
    subprocess.run([str(args.executable.resolve()), str(native)], check=True, timeout=30)
    subprocess.run([str(args.executable.resolve()), "--roundtrip", str(native), str(native_copy)], check=True, timeout=30)
    for path in (native, native_copy):
        decoded = sd.read_zarr(path)
        assert [tuple(level.shape) for level in levels(decoded.images["odd"])] == [(1, 5, 5), (1, 3, 3)]
        group = zarr.open_group(path, mode="r")
        transformations = group["images/odd"].attrs["ome"]["multiscales"][0]["datasets"][1]["coordinateTransformations"]
        assert transformations[0] == {"type": "scale", "scale": [1.0, 2.0, 2.0]}
        assert transformations[1] == {"type": "translation", "translation": [0.0, 0.5, 0.5]}
    print("PASS odd pyramid: scale/translation survive native and independent reopen", flush=True)
    cases = []
    for encoding in ("wkb", "geoarrow"):
        destination = run / encoding
        command = [sys.executable, str(source / "tests/reference/generate_spatial_current.py"), str(destination)]
        if encoding == "geoarrow":
            command.append("--geoarrow")
        subprocess.run(command, check=True, timeout=45)
        cases.append((encoding, destination / "fixture.zarr"))
    for name, explicit, default in (
        ("legacy02", args.legacy02, source / "build/reference/legacy-spatial-02/fixture.zarr"),
        ("legacy03", args.legacy03, source / "build/reference/legacy-spatial-03/fixture.zarr"),
    ):
        fixture = explicit or default
        if not fixture.is_dir():
            raise RuntimeError(f"required {name} fixture is absent: {fixture}")
        cases.append((name, fixture))
    for name, fixture in cases:
        output = run / (name + "-cpp.zarr")
        subprocess.run([str(args.executable.resolve()), "--roundtrip", str(fixture.resolve()), str(output)],
                       check=True, timeout=30)
        before, after = sd.read_zarr(fixture), sd.read_zarr(output)
        compare(before, after)
        print(f"PASS {name}: all element payloads, IDs, categories, links and transforms", flush=True)
    print(f"Independent SpatialData roundtrip artifacts: {run}", flush=True)


if __name__ == "__main__":
    main()
