from __future__ import annotations

import argparse
from hashlib import sha256
import importlib.metadata as metadata
import json
from pathlib import Path
import sys

import anndata as ad
import geopandas as gpd
import numpy as np
import pandas as pd
import spatialdata as sd
import spatialdata._io.format as formats
import zarr
from shapely.geometry import MultiPolygon, Point, Polygon
from spatialdata import models


def _shape_details(shape: gpd.GeoDataFrame) -> dict[str, object]:
    geometry = shape.geometry.iloc[0]
    result: dict[str, object] = {
        "index_dtype": str(shape.index.dtype),
        "geometry_type": geometry.geom_type,
    }
    if geometry.geom_type == "Polygon":
        result["interior_rings"] = len(geometry.interiors)
    elif geometry.geom_type == "MultiPolygon":
        result["parts"] = len(geometry.geoms)
    if "radius" in shape:
        result["radius_dtype"] = str(shape["radius"].dtype)
        result["radius"] = float(shape["radius"].iloc[0])
    return result


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Write and reopen a small SpatialData legacy-format Zarr store."
    )
    parser.add_argument(
        "output_directory",
        type=Path,
        help="New output directory for fixture.zarr and fixture-summary.json.",
    )
    args = parser.parse_args()

    output_dir = args.output_directory.expanduser().resolve()
    store_path = output_dir / "fixture.zarr"
    summary_path = output_dir / "fixture-summary.json"
    manifest_path = output_dir / "fixture-files.sha256"
    if store_path.exists():
        raise SystemExit(f"Refusing to overwrite existing store: {store_path}")
    if summary_path.exists():
        raise SystemExit(f"Refusing to overwrite existing summary: {summary_path}")
    if manifest_path.exists():
        raise SystemExit(f"Refusing to overwrite existing manifest: {manifest_path}")
    output_dir.mkdir(parents=True, exist_ok=True)

    image = models.Image2DModel.parse(
        np.arange(16, dtype=np.uint16).reshape(1, 4, 4),
        dims=("c", "y", "x"),
        c_coords=["DAPI"],
    )
    labels = models.Labels2DModel.parse(
        np.asarray(
            [
                [0, 0, 1, 1],
                [0, 0, 1, 1],
                [2, 2, 0, 0],
                [2, 2, 0, 0],
            ],
            dtype=np.uint16,
        ),
        dims=("y", "x"),
    )

    point_frame = pd.DataFrame(
        {
            "x": np.asarray([0.5, 2.5, 7.5], dtype=np.float64),
            "y": np.asarray([0.5, 2.5, 7.5], dtype=np.float64),
            "point_id": np.asarray([9001, 9002, 9003], dtype=np.int64),
            "feature": pd.Categorical(
                ["transcript", "landmark", "transcript"],
                categories=["landmark", "transcript", "background"],
                ordered=True,
            ),
        }
    )
    points = models.PointsModel.parse(
        point_frame,
        feature_key="feature",
        instance_key="point_id",
    )

    polygon_with_hole = Polygon(
        shell=[(0, 0), (4, 0), (4, 4), (0, 4), (0, 0)],
        holes=[[(1, 1), (2, 1), (2, 2), (1, 2), (1, 1)]],
    )
    hole_shape = models.ShapesModel.parse(
        gpd.GeoDataFrame(
            {"kind": ["polygon-with-hole"]},
            geometry=[polygon_with_hole],
            index=pd.Index(np.asarray([101], dtype=np.int64), name="region_id"),
        )
    )
    single_part_multipolygon = MultiPolygon(
        [Polygon([(5, 5), (6, 5), (6, 6), (5, 6), (5, 5)])]
    )
    multipolygon_shape = models.ShapesModel.parse(
        gpd.GeoDataFrame(
            {"kind": ["single-part-multipolygon"]},
            geometry=[single_part_multipolygon],
            index=pd.Index(np.asarray([202], dtype=np.int64), name="region_id"),
        )
    )
    circle_shape = models.ShapesModel.parse(
        gpd.GeoDataFrame(
            {"radius": np.asarray([0.75], dtype=np.float64)},
            geometry=[Point(8.5, 8.5)],
            index=pd.Index(np.asarray([303], dtype=np.int64), name="region_id"),
        )
    )

    table_data = ad.AnnData(
        X=np.asarray(
            [[1.0, 0.0], [0.0, 2.0], [1.0, 1.0]],
            dtype=np.float32,
        ),
        obs=pd.DataFrame(
            {
                "region": pd.Categorical(
                    ["polygon_with_hole", "single_part_multipolygon", "circles"],
                    categories=["polygon_with_hole", "single_part_multipolygon", "circles"],
                ),
                "region_id": np.asarray([101, 202, 303], dtype=np.int64),
            },
            index=["obs-101", "obs-202", "obs-303"],
        ),
        var=pd.DataFrame(index=["gene-a", "gene-b"]),
    )
    table = models.TableModel.parse(
        table_data,
        region=["polygon_with_hole", "single_part_multipolygon", "circles"],
        region_key="region",
        instance_key="region_id",
    )

    sdata = sd.SpatialData(
        images={"image": image},
        labels={"segmentation": labels},
        points={"points": points},
        shapes={
            "polygon_with_hole": hole_shape,
            "single_part_multipolygon": multipolygon_shape,
            "circles": circle_shape,
        },
        tables={"table": table},
    )
    sdata.write(store_path)

    manifest_entries = []
    for file_path in sorted(path for path in store_path.rglob("*") if path.is_file()):
        relative_path = file_path.relative_to(store_path).as_posix()
        manifest_entries.append(f"{sha256(file_path.read_bytes()).hexdigest()}  {relative_path}")
    manifest_text = "\n".join(manifest_entries) + "\n"
    with manifest_path.open("x") as stream:
        stream.write(manifest_text)
    manifest_sha256 = sha256(manifest_text.encode()).hexdigest()

    reopened = sd.read_zarr(store_path)
    reopened_points = reopened.points["points"].compute()
    reopened_hole = reopened.shapes["polygon_with_hole"]
    reopened_multi = reopened.shapes["single_part_multipolygon"]
    reopened_circles = reopened.shapes["circles"]
    reopened_table = reopened.tables["table"]
    hole_geometry = reopened_hole.geometry.iloc[0]
    multi_geometry = reopened_multi.geometry.iloc[0]
    circle_geometry = reopened_circles.geometry.iloc[0]
    point_features = sorted(reopened_points["feature"].astype(str).unique().tolist())
    table_regions = sorted(reopened_table.obs["region"].astype(str).unique().tolist())
    table_links = []
    for region, region_id in zip(
        reopened_table.obs["region"].astype(str),
        reopened_table.obs["region_id"],
        strict=True,
    ):
        table_links.append(
            {
                "region": region,
                "region_id": int(region_id),
                "shape_exists": int(region_id) in reopened.shapes[region].index,
            }
        )
    table_links_valid = all(link["shape_exists"] for link in table_links)

    readback = {
        "images": list(reopened.images),
        "labels": list(reopened.labels),
        "labels_shape": list(reopened.labels["segmentation"].shape),
        "points": list(reopened.points),
        "point_rows": len(reopened_points),
        "point_id_dtype": str(reopened_points["point_id"].dtype),
        "point_feature_dtype": str(reopened_points["feature"].dtype),
        "point_feature_values": point_features,
        "shapes": list(reopened.shapes),
        "polygon_with_hole": _shape_details(reopened_hole),
        "single_part_multipolygon": _shape_details(reopened_multi),
        "circles": _shape_details(reopened_circles),
        "table_shape": list(reopened_table.shape),
        "table_region_dtype": str(reopened_table.obs["region"].dtype),
        "table_region_values": table_regions,
        "table_instance_id_dtype": str(reopened_table.obs["region_id"].dtype),
        "table_links": table_links,
        "table_links_valid": table_links_valid,
    }
    assert readback["images"] == ["image"]
    assert readback["labels"] == ["segmentation"] and readback["labels_shape"] == [4, 4]
    assert readback["points"] == ["points"] and readback["point_rows"] == 3
    assert readback["point_id_dtype"] == "int64"
    assert readback["point_feature_dtype"] == "category"
    assert point_features == ["landmark", "transcript"]
    assert set(readback["shapes"]) == {"polygon_with_hole", "single_part_multipolygon", "circles"}
    assert hole_geometry.geom_type == "Polygon" and len(hole_geometry.interiors) == 1
    assert multi_geometry.geom_type == "MultiPolygon" and len(multi_geometry.geoms) == 1
    assert circle_geometry.geom_type == "Point" and float(reopened_circles["radius"].iloc[0]) == 0.75
    assert list(reopened_hole.index) == [101]
    assert list(reopened_multi.index) == [202]
    assert list(reopened_circles.index) == [303]
    assert readback["table_shape"] == [3, 2]
    assert readback["table_region_dtype"] == "category"
    assert table_regions == ["circles", "polygon_with_hole", "single_part_multipolygon"]
    assert readback["table_instance_id_dtype"] == "int64"
    assert table_links_valid

    group = zarr.open_group(str(store_path), mode="r")
    element_attrs = {}
    for element_path in (
        "images/image",
        "labels/segmentation",
        "points/points",
        "shapes/polygon_with_hole",
        "shapes/single_part_multipolygon",
        "shapes/circles",
        "tables/table",
    ):
        element_attrs[element_path] = group[element_path].attrs.asdict()

    format_summary = {}
    for label, cls_name in (
        ("container", "CurrentSpatialDataContainerFormats"),
        ("image", "CurrentRasterFormat"),
        ("points", "CurrentPointsFormat"),
        ("shapes", "CurrentShapesFormat"),
        ("table", "CurrentTablesFormat"),
    ):
        instance = getattr(formats, cls_name)()
        item = {
            "class": type(instance).__name__,
            "spatialdata_format_version": instance.spatialdata_format_version,
        }
        if hasattr(instance, "version"):
            item["ome_zarr_format_version"] = instance.version
        format_summary[label] = item
    raster_format = formats.CurrentRasterFormat()
    format_summary["labels"] = {
        "class": type(raster_format).__name__,
        "spatialdata_format_version": raster_format.spatialdata_format_version,
        "shared_writer": "CurrentRasterFormat",
    }
    if hasattr(raster_format, "version"):
        format_summary["labels"]["ome_zarr_format_version"] = raster_format.version

    package_names = (
        "spatialdata",
        "anndata",
        "zarr",
        "geopandas",
        "datashader",
        "numpy",
        "pandas",
        "setuptools",
        "ome-zarr",
    )
    package_versions = {}
    for name in package_names:
        try:
            package_versions[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            pass

    summary = {
        "python": f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
        "packages": package_versions,
        "store": str(store_path),
        "fixture_manifest": {
            "path": str(manifest_path),
            "file_count": len(manifest_entries),
            "sha256": manifest_sha256,
        },
        "elements": {
            "images": list(sdata.images),
            "labels": list(sdata.labels),
            "points": list(sdata.points),
            "shapes": list(sdata.shapes),
            "tables": list(sdata.tables),
        },
        "readback": readback,
        "formats": format_summary,
        "root_attrs": group.attrs.asdict(),
        "element_attrs": element_attrs,
    }
    with summary_path.open("x") as stream:
        stream.write(json.dumps(summary, indent=2, default=str) + "\n")
    print(json.dumps(summary, indent=2, default=str))


if __name__ == "__main__":
    main()
