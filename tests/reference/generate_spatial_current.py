#!/usr/bin/env python3
"""Write and independently reopen a current SpatialData fixture.

Usage: python tests/reference/generate_spatial_current.py OUTPUT [--geoarrow]
OUTPUT must not already exist. Set ANNDATAPP_INTEROP_ZARR_WAKEUP=1 only on the
documented restricted test runner; normal hosts use the ordinary event loop.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import sys

if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") == "1":
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from python_storage import install_restricted_runner_wakeup  # noqa: F401

import anndata as ad
import geopandas as gpd
import numpy as np
import pandas as pd
from shapely.geometry import MultiPolygon, Point, Polygon, box
from spatialdata import SpatialData, read_zarr
from spatialdata.models import Image2DModel, Labels2DModel, PointsModel, ShapesModel, TableModel
from spatialdata.transformations import Scale, Translation


def fixture() -> SpatialData:
    image = Image2DModel.parse(
        np.arange(64, dtype=np.uint16).reshape(1, 8, 8),
        dims=("c", "y", "x"), c_coords=["DAPI"], scale_factors=[2],
    )
    labels = Labels2DModel.parse(
        np.array([[0, 1, 1, 0], [0, 1, 1, 0], [0, 2, 2, 0], [0, 2, 2, 0]], dtype=np.uint32),
        dims=("y", "x"),
    )
    points = PointsModel.parse(
        pd.DataFrame({"x": [0.5, 1.5, 2.5], "y": [0.5, 1.5, 2.5],
                      "spot_id": np.array([101, 102, 103], dtype=np.int64),
                      "gene": pd.Categorical(["A", "B", "A"])}),
        instance_key="spot_id", transformations={"global": Translation([1.25, -2.5], axes=("x", "y"))},
    )
    polygon = Polygon([(0, 0), (3, 0), (3, 3), (0, 3)],
                      holes=[[(1, 1), (2, 1), (2, 2), (1, 2)]])
    polygons = ShapesModel.parse(
        gpd.GeoDataFrame({"kind": pd.Categorical(["with-hole"])}, geometry=[polygon],
                         index=pd.Index(np.array([11], dtype=np.int64), name="instance_id")),
        transformations={"global": Scale([1.5, 2.0], axes=("x", "y"))},
    )
    multipolygons = ShapesModel.parse(
        gpd.GeoDataFrame({"kind": ["single", "multiple"]},
                         geometry=[MultiPolygon([box(4, 0, 5, 1)]),
                                   MultiPolygon([box(6, 0, 7, 1), box(8, 0, 9, 1)])],
                         index=pd.Index(np.array([31, 32], dtype=np.int64), name="instance_id")),
    )
    circles = ShapesModel.parse(
        gpd.GeoDataFrame({"radius": [0.5, 0.75]}, geometry=[Point(1, 2), Point(3, 4)],
                         index=pd.Index(np.array([21, 22], dtype=np.int64), name="instance_id")),
    )
    data = ad.AnnData(
        X=np.array([[2.0, 0.0], [0.0, 3.0]], dtype=np.float32),
        obs=pd.DataFrame({"region": pd.Categorical(["cells", "cells"]),
                          "instance_id": np.array([1, 2], dtype=np.uint32)}, index=["c1", "c2"]),
        var=pd.DataFrame(index=["g1", "g2"]),
    )
    table = TableModel.parse(data, region="cells", region_key="region", instance_key="instance_id")
    return SpatialData(images={"image": image}, labels={"cells": labels}, points={"points": points},
                       shapes={"polygons": polygons, "multipolygons": multipolygons, "circles": circles},
                       tables={"table": table})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new output directory; must not already exist")
    parser.add_argument("--geoarrow", action="store_true", help="write native GeoArrow geometry instead of WKB")
    args = parser.parse_args()
    if metadata.version("spatialdata") != "0.9.0":
        raise RuntimeError("this fixture requires spatialdata 0.9.0")
    args.output.mkdir(parents=True, exist_ok=False)
    path = args.output / "fixture.zarr"
    sdata = fixture()
    sdata.write(path, overwrite=False, shapes_geometry_encoding="geoarrow" if args.geoarrow else "WKB")
    got = read_zarr(path)
    points = got.points["points"].compute()
    assert set(got.images) == {"image"} and set(got.labels) == {"cells"}
    assert set(got.points) == {"points"} and set(got.tables) == {"table"}
    assert set(got.shapes) == {"polygons", "multipolygons", "circles"}
    assert len(points) == 3 and list(points["spot_id"]) == [101, 102, 103]
    assert list(points["gene"].astype(str)) == ["A", "B", "A"]
    assert len(got.shapes["polygons"].geometry.iloc[0].interiors) == 1
    assert [len(g.geoms) for g in got.shapes["multipolygons"].geometry] == [1, 2]
    assert list(got.shapes["circles"]["radius"]) == [0.5, 0.75]
    assert got.tables["table"].shape == (2, 2)
    packages = ("spatialdata", "anndata", "zarr", "geopandas", "pyarrow", "numpy", "pandas")
    summary = {"python": sys.version.split()[0], "packages": {p: metadata.version(p) for p in packages},
               "geometry_encoding": "geoarrow" if args.geoarrow else "WKB",
               "images": sorted(got.images), "labels": sorted(got.labels), "points": sorted(got.points),
               "shapes": sorted(got.shapes), "tables": sorted(got.tables), "table_shape": [2, 2]}
    (args.output / "fixture-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    lines = []
    for file in sorted(path.rglob("*")):
        if file.is_file():
            lines.append(f"{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.relative_to(path)}")
    manifest = args.output / "fixture-files.sha256"
    manifest.write_text("\n".join(lines) + "\n")
    print(json.dumps(summary, sort_keys=True))
    print("manifest_sha256:", hashlib.sha256(manifest.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
