#!/usr/bin/env python3
"""Generate and inspect pinned TreeData interoperability fixtures.

Current files are written and reopened by treedata 0.4.0. A helper subprocess
writes the historical scalar-JSON Zarr fixture using treedata 0.2.5 directly.

Example:
    python tests/treedata_reference.py --out build/reference/treedata \
        --legacy-python build/reference/legacy-tree/bin/python
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import warnings
from typing import Any

ALIGNMENTS = ("leaves", "nodes", "subset")
DEFAULT_LABEL = "tree_key"


def prepare_runtime() -> None:
    if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") == "1":
        from importlib.metadata import version
        if int(version("zarr").split(".")[0]) < 3:
            return  # Zarr 2 uses synchronous store operations.
        tests = str(Path(__file__).resolve().parent)
        if tests not in sys.path:
            sys.path.insert(0, tests)
        from python_storage import install_restricted_runner_wakeup
        install_restricted_runner_wakeup()


def versions() -> dict[str, str]:
    from importlib.metadata import version
    return {name: version(name) for name in ("treedata", "anndata", "zarr", "numpy", "h5py")}


def require_version(expected: str) -> dict[str, str]:
    result = versions()
    if result["treedata"] != expected:
        raise RuntimeError(
            f"expected treedata=={expected}, found {result['treedata']}; "
            "activate the matching reference environment"
        )
    return result


def make_tree(nx: Any, prefix: str, with_inner: bool) -> Any:
    graph = nx.DiGraph()
    root, leaf_a, leaf_b = f"{prefix}-root", f"{prefix}-a", f"{prefix}-b"
    graph.add_node(root, depth=0, active=True,
                   payload={"kind": "root", "trail": [prefix, 0]}, nullable=None)
    if with_inner:
        inner = f"{prefix}-inner"
        graph.add_node(inner, depth=1, active=False,
                       payload=["branch", 1, {"kept": True}], nullable="present")
        graph.add_node(leaf_a, depth=2, active=True, payload="leaf-a")
        graph.add_node(leaf_b, depth=2, active=False,
                       payload={"kind": "leaf", "ordinal": 2}, nullable=[None, 2.5])
        graph.add_edge(root, inner, length=0.5, reliable=True, note="trunk")
        graph.add_edge(inner, leaf_a, length=1.25, reliable=False, note=None)
        graph.add_edge(inner, leaf_b, length=1.5, reliable=True)
    else:
        graph.add_node(leaf_a, depth=1, active=False,
                       payload=["leaf", prefix, 1])
        graph.add_node(leaf_b, depth=1, active=True,
                       payload={"kind": "leaf", "ordinal": 2}, nullable="present")
        graph.add_edge(root, leaf_a, length=0.25, reliable=False, note=None)
        graph.add_edge(root, leaf_b, length=0.75, reliable=True)
    return graph


def make_graphs() -> tuple[dict[str, Any], dict[str, Any]]:
    import networkx as nx
    return (
        {"alpha": make_tree(nx, "alpha", True),
         "beta": make_tree(nx, "beta", False)},
        {"genes": make_tree(nx, "gene", True)},
    )


def axis_names(trees: dict[str, Any], alignment: str, axis: str) -> list[str]:
    if alignment == "subset":
        return ["alpha-root", "alpha-a", "beta-b"] if axis == "obs" else ["gene-root", "gene-b"]
    result = []
    for graph in trees.values():
        for node in graph.nodes:
            if alignment == "nodes" or graph.out_degree(node) == 0:
                result.append(str(node))
    return result


def make_treedata(alignment: str = "leaves", *, label: str | None = DEFAULT_LABEL, raw: Any = None) -> Any:
    import numpy as np
    import pandas as pd
    from treedata import TreeData

    obst, vart = make_graphs()
    obs_names, var_names = axis_names(obst, alignment, "obs"), axis_names(vart, alignment, "var")
    obs = pd.DataFrame({"obs_rank": np.arange(len(obs_names), dtype=np.int32)}, index=obs_names)
    var = pd.DataFrame({"var_rank": np.arange(len(var_names), dtype=np.int16)}, index=var_names)
    X = np.arange(len(obs_names) * len(var_names), dtype=np.float32).reshape(len(obs_names), len(var_names))
    return TreeData(X=X, obs=obs, var=var, obst=obst, vart=vart, raw=raw, label=label,
                    alignment=alignment, allow_overlap=False)


def canonical(value: Any) -> Any:
    if hasattr(value, "item") and callable(value.item):
        try:
            return canonical(value.item())
        except (ValueError, TypeError):
            pass
    if isinstance(value, dict):
        return {str(k): canonical(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [canonical(v) for v in value]
    return value


def graph_signature(graph: Any) -> dict[str, Any]:
    return {
        "nodes": [(str(n), canonical(attrs)) for n, attrs in graph.nodes(data=True)],
        "edges": [(str(u), str(v), canonical(attrs)) for u, v, attrs in graph.edges(data=True)],
    }


def tree_signatures(trees: Any) -> dict[str, Any]:
    return {str(key): graph_signature(trees[key]) for key in sorted(trees.keys())}


def assert_graphs_equal(expected: Any, actual: Any, context: str) -> None:
    for axis in ("obst", "vart"):
        want, got = tree_signatures(getattr(expected, axis)), tree_signatures(getattr(actual, axis))
        assert got == want, f"{context}: {axis} differs; expected {want!r}, got {got!r}"


def expected_labels(trees: Any, names: list[str], alignment: str, allow_overlap: bool) -> list[str | None]:
    result = []
    for name in names:
        keys = []
        for key in trees:
            graph = trees[key]
            eligible = [n for n in graph if alignment != "leaves" or graph.out_degree(n) == 0]
            if name in eligible:
                keys.append(str(key))
        result.append(",".join(sorted(keys)) if allow_overlap and keys else
                      (keys[0] if keys else None))
    return result


def column_values(table: Any, key: str) -> list[str | None]:
    import pandas as pd
    return [None if pd.isna(x) else str(x) for x in table[key].to_numpy(dtype=object)]


def assert_label_columns(data: Any, context: str) -> None:
    if data.label is None:
        assert data.label not in data.obs.columns
        assert data.label not in data.var.columns
        return
    for trees, table in ((data.obst, data.obs), (data.vart, data.var)):
        assert data.label in table.columns, f"{context}: missing label column {data.label!r}"
        want = expected_labels(trees, [str(n) for n in table.index], data.alignment, data.allow_overlap)
        got = column_values(table, data.label)
        assert got == want, f"{context}: labels {got!r} != {want!r}"


def assert_roundtrip(expected: Any, actual: Any, context: str) -> None:
    import numpy as np
    assert actual.alignment == expected.alignment, f"{context}: alignment differs"
    assert actual.allow_overlap == expected.allow_overlap, f"{context}: allow_overlap differs"
    assert actual.label == expected.label, f"{context}: label differs"
    assert [str(n) for n in actual.obs_names] == [str(n) for n in expected.obs_names]
    assert [str(n) for n in actual.var_names] == [str(n) for n in expected.var_names]
    np.testing.assert_array_equal(np.asarray(actual.X), np.asarray(expected.X))
    assert (actual.raw is None) == (expected.raw is None), f"{context}: raw presence differs"
    if expected.raw is not None:
        np.testing.assert_array_equal(np.asarray(actual.raw.X), np.asarray(expected.raw.X))
        assert [str(n) for n in actual.raw.var_names] == [str(n) for n in expected.raw.var_names]
        assert actual.raw.var_names.name == expected.raw.var_names.name
        assert list(actual.raw.var.columns) == list(expected.raw.var.columns)
        for column in expected.raw.var.columns:
            want = [canonical(value) for value in expected.raw.var[column].astype(object).tolist()]
            got = [canonical(value) for value in actual.raw.var[column].astype(object).tolist()]
            assert got == want, f"{context}: raw.var[{column!r}] differs"
    assert_graphs_equal(expected, actual, context)
    assert_label_columns(actual, context)


def jsonable_graph(graph: Any) -> dict[str, Any]:
    return {
        "nodes": {str(n): canonical(attrs) for n, attrs in graph.nodes(data=True)},
        "edges": {str(n): {str(v): canonical(attrs) for v, attrs in graph[n].items()} for n in graph},
    }


def expected_json_trees(trees: Any) -> dict[str, Any]:
    return {str(key): jsonable_graph(trees[key]) for key in trees}


def text(value: Any) -> str:
    return value.decode("utf-8") if isinstance(value, bytes) else str(value)


def read_scalar(group: Any, key: str, ad: Any) -> Any:
    return ad.io.read_elem(group[key])


def assert_root_metadata(group: Any, expected: Any, ad: Any, context: str) -> None:
    attrs = dict(group.attrs)
    assert text(attrs["encoding-type"]) == "treedata", f"{context}: root encoding-type"
    assert text(attrs["encoding-version"]) == "0.1.0", f"{context}: root encoding-version"
    assert read_scalar(group, "label", ad) == expected.label, f"{context}: label element"
    assert read_scalar(group, "alignment", ad) == expected.alignment, f"{context}: alignment element"
    assert bool(read_scalar(group, "allow_overlap", ad)) == expected.allow_overlap


def assert_hdf5(path: Path, expected: Any) -> None:
    import anndata as ad
    import h5py
    with h5py.File(path, "r") as group:
        assert_root_metadata(group, expected, ad, str(path))
        for axis in ("obst", "vart"):
            actual = json.loads(group[axis].asstr()[()])
            assert actual == expected_json_trees(getattr(expected, axis)), f"{path}: {axis} JSON differs"


def stored_attr_strings(array: Any) -> list[str]:
    return [text(value) for value in array[:]]


def assert_zarr_attr_columns(group: Any, key: str, graph: Any, axis: str) -> None:
    import numpy as np
    tree = group[axis][key]
    nodes = [text(value) for value in tree["nodes"][:]]
    edges = [tuple(text(value) for value in pair) for pair in tree["edges"][:]]
    node_attrs, edge_attrs = tree["node_attrs"], tree["edge_attrs"]
    assert np.asarray(node_attrs["depth"][:]).dtype.kind in "iu"
    assert np.asarray(node_attrs["active"][:]).dtype.kind == "b"
    assert np.asarray(edge_attrs["length"][:]).dtype.kind == "f"
    assert np.asarray(edge_attrs["reliable"][:]).dtype.kind == "b"
    nullable = dict(zip(nodes, stored_attr_strings(node_attrs["nullable"]), strict=True))
    prefix = "gene" if key == "genes" else key
    assert nullable[f"{prefix}-root"] == "null"
    assert nullable[f"{prefix}-a"] == ""
    notes = dict(zip(edges, stored_attr_strings(edge_attrs["note"]), strict=True))
    if key == "beta":
        assert notes[("beta-root", "beta-a")] == "null"
        assert notes[("beta-root", "beta-b")] == ""
    else:
        assert notes[(f"{prefix}-inner", f"{prefix}-a")] == "null"
        assert notes[(f"{prefix}-inner", f"{prefix}-b")] == ""
    payload = stored_attr_strings(node_attrs["payload"])
    assert len(payload) == len(nodes) == graph.number_of_nodes()
    assert json.loads(payload[0])["kind"] == "root"
    assert json.loads(payload[1])[0] == ("leaf" if key == "beta" else "branch")


def assert_zarr(path: Path, expected: Any) -> None:
    import anndata as ad
    import zarr
    group = zarr.open_group(str(path), mode="r")
    assert_root_metadata(group, expected, ad, str(path))
    for axis in ("obst", "vart"):
        trees = getattr(expected, axis)
        stored = group[axis]
        assert set(stored.keys()) == set(trees.keys()), f"{path}: {axis} keys"
        for key in trees:
            tree = stored[key]
            graph = trees[key]
            nodes = [text(n) for n in tree["nodes"][:]]
            edges = [tuple(text(v) for v in pair) for pair in tree["edges"][:]]
            assert nodes == [str(n) for n in graph.nodes], f"{path}: {axis}.{key} node order"
            assert edges == [(str(u), str(v)) for u, v in graph.edges], f"{path}: {axis}.{key} edge order"
            assert_zarr_attr_columns(group, key, graph, axis)


def assert_selection_semantics() -> dict[str, Any]:
    leaves = make_treedata("leaves")
    selected = leaves[["alpha-a", "beta-b"], ["gene-a"]]
    assert list(selected.obst["alpha"].nodes) == ["alpha-root", "alpha-inner", "alpha-a"]
    assert list(selected.obst["beta"].nodes) == ["beta-root", "beta-b"]
    assert list(selected.vart["genes"].nodes) == ["gene-root", "gene-inner", "gene-a"]

    nodes = make_treedata("nodes")
    connected = nodes[["alpha-root", "alpha-inner", "alpha-a"], ["gene-root", "gene-inner", "gene-a"]]
    assert connected.alignment == "nodes"
    assert list(connected.obst["alpha"].nodes) == ["alpha-root", "alpha-inner", "alpha-a"]
    assert list(connected.vart["genes"].nodes) == ["gene-root", "gene-inner", "gene-a"]
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        disconnected = nodes[["alpha-root", "alpha-a"], ["gene-root", "gene-b"]]
    assert disconnected.alignment == "subset"
    assert list(disconnected.obst["alpha"].nodes) == ["alpha-root", "alpha-inner", "alpha-a", "alpha-b"]
    assert list(disconnected.vart["genes"].nodes) == ["gene-root", "gene-inner", "gene-a", "gene-b"]

    subset = make_treedata("subset")
    subset_slice = subset[["alpha-root", "beta-b"], ["gene-root"]]
    assert subset_slice.alignment == "subset"
    assert tree_signatures(subset_slice.obst) == tree_signatures(subset.obst)
    assert tree_signatures(subset_slice.vart) == tree_signatures(subset.vart)
    return {
        "leaves_slice": {"alpha": list(selected.obst["alpha"].nodes),
                         "beta": list(selected.obst["beta"].nodes),
                         "genes": list(selected.vart["genes"].nodes)},
        "connected_nodes_alignment": connected.alignment,
        "disconnected_nodes_alignment": disconnected.alignment,
        "subset_slice_retains_full_topology": True,
    }


def assert_label_semantics(_directory: Path) -> dict[str, Any]:
    import anndata as ad
    import h5py
    import numpy as np
    import pandas as pd
    import zarr
    from treedata import TreeData, read_h5td, read_zarr

    graph = make_graphs()[0]["alpha"]
    obst, vart = make_graphs()
    obs_names, var_names = axis_names(obst, "leaves", "obs"), axis_names(vart, "leaves", "var")
    data = TreeData(
        X=np.zeros((len(obs_names), len(var_names)), dtype=np.float32),
        obs=pd.DataFrame(index=obs_names), var=pd.DataFrame(index=var_names),
        obst={}, vart={}, label="group", alignment="leaves", allow_overlap=True,
    )
    assert "group" not in data.obs.columns
    data.obst["zeta"] = graph
    assert column_values(data.obs, "group")[:2] == ["zeta", "zeta"]
    data.obst["alpha"] = graph.copy()
    assert column_values(data.obs, "group")[:2] == ["alpha,zeta", "alpha,zeta"]
    assert data.has_overlap
    del data.obst["zeta"]
    assert column_values(data.obs, "group")[:2] == ["alpha", "alpha"]
    del data.obst["alpha"]
    assert all(pd.isna(value) for value in data.obs["group"])

    no_label = make_treedata("leaves", label=None)
    assert no_label.label is None and list(no_label.obs.columns) == ["obs_rank"]
    no_label.label = "late"
    assert "late" not in no_label.obs.columns
    no_label.obst["alpha"] = graph
    assert column_values(no_label.obs, "late")[:2] == ["alpha", "alpha"]

    empty_h5_error = ""
    empty_zarr_error = ""
    with tempfile.TemporaryDirectory(prefix="treedata-label-reference-") as temp:
        label_dir = Path(temp)
        source = make_treedata("leaves", label=None)
        h5_path, zarr_path = label_dir / "none.h5td", label_dir / "none.zarr"
        source.write_h5td(h5_path)
        source.write_zarr(str(zarr_path))
        for readback in (read_h5td(h5_path), read_zarr(zarr_path)):
            assert readback.label is None
            assert_label_columns(readback, str(h5_path))
        with h5py.File(h5_path, "r") as store:
            assert "label" in store and read_scalar(store, "label", ad) is None
        zgroup = zarr.open_group(str(zarr_path), mode="r")
        assert "label" in zgroup and read_scalar(zgroup, "label", ad) is None

        source = make_treedata("leaves", label="")
        empty_zarr = label_dir / "empty.zarr"
        source.write_zarr(str(empty_zarr))
        empty_group = zarr.open_group(str(empty_zarr), mode="r")
        assert read_scalar(empty_group, "label", ad) == ""
        try:
            empty_readback = read_zarr(empty_zarr)
        except Exception as error:
            empty_zarr_error = f"{type(error).__name__}: {error}"
        else:
            assert empty_readback.label == ""
            assert_label_columns(empty_readback, str(empty_zarr))
        try:
            source.write_h5td(label_dir / "empty.h5td")
        except Exception as error:
            empty_h5_error = f"{type(error).__name__}: {error}"
        else:
            raise AssertionError("pinned AnnData/TreeData unexpectedly wrote an empty-named HDF5 obs column")

        # An absent label element is distinct from a stored null or empty string.
        source = make_treedata("leaves")
        absent_h5, absent_zarr = label_dir / "absent.h5td", label_dir / "absent.zarr"
        source.write_h5td(absent_h5)
        source.write_zarr(str(absent_zarr))
        with h5py.File(absent_h5, "r+") as store:
            del store["label"]
        zgroup = zarr.open_group(str(absent_zarr), mode="a")
        del zgroup["label"]
        for readback in (read_h5td(absent_h5), read_zarr(absent_zarr)):
            assert readback.label == "tree"
            assert column_values(readback.obs, "tree") == ["alpha", "alpha", "beta", "beta"]
            assert "tree_key" in readback.obs.columns
            assert column_values(readback.var, "tree") == ["genes", "genes"]

    return {
        "overlap_label_order": "alpha,zeta",
        "remove_one_tree_label": "remaining key",
        "remove_all_trees_label": "missing values in retained column",
        "none_label": "explicit null; no generated label column",
        "empty_label": "empty string; creates an empty-named label column in memory",
        "empty_label_zarr": f"reader failure observed: {empty_zarr_error}",
        "empty_label_hdf5": f"writer failure observed: {empty_h5_error}",
        "absent_label": "defaults to 'tree' and synthesizes obs/var labels",
        "label_setter": "does not backfill until a tree mapping changes",
    }


def write_current(out: Path) -> None:
    for alignment in ALIGNMENTS:
        data = make_treedata(alignment)
        h5 = out / "hdf5" / f"treedata-{alignment}.h5td"
        zarr_path = out / "zarr" / f"treedata-{alignment}.zarr"
        h5.parent.mkdir(parents=True, exist_ok=True)
        zarr_path.parent.mkdir(parents=True, exist_ok=True)
        data.write_h5td(h5)
        data.write_zarr(str(zarr_path))


def legacy_helper(path: Path) -> None:
    prepare_runtime()
    found = require_version("0.2.5")
    path.parent.mkdir(parents=True, exist_ok=True)
    make_treedata("leaves").write_zarr(str(path))
    print(json.dumps({"legacy_versions": found, "path": str(path)}, sort_keys=True))


def write_legacy(path: Path, interpreter: Path) -> None:
    result = subprocess.run(
        [str(interpreter), str(Path(__file__).resolve()), "--_legacy-helper", str(path)],
        text=True, capture_output=True, check=False,
    )
    if result.returncode:
        raise RuntimeError(f"legacy writer failed: {result.stderr.strip()}\n{result.stdout.strip()}")
    if result.stdout.strip():
        print(result.stdout.strip())


def assert_legacy(path: Path, expected: Any) -> None:
    import anndata as ad
    import zarr
    from treedata import read_zarr
    group = zarr.open_group(str(path), mode="r")
    assert text(group.attrs["encoding-type"]) == "treedata"
    for axis in ("obst", "vart"):
        element = group[axis]
        assert isinstance(element, zarr.Array) and element.shape == (), f"{path}: expected scalar {axis}"
        raw_json = text(ad.io.read_elem(element))
        assert json.loads(raw_json) == expected_json_trees(getattr(expected, axis))
    assert_roundtrip(expected, read_zarr(path), f"0.4 reader opening legacy {path}")


def hash_path(path: Path) -> str:
    digest = hashlib.sha256()
    if path.is_file():
        digest.update(path.read_bytes())
    else:
        for child in sorted(item for item in path.rglob("*") if item.is_file()):
            digest.update(str(child.relative_to(path)).encode())
            digest.update(b"\0")
            digest.update(child.read_bytes())
    return digest.hexdigest()


def inspect(out: Path) -> dict[str, Any]:
    from treedata import read_h5td, read_zarr
    files = []
    for alignment in ALIGNMENTS:
        expected = make_treedata(alignment)
        h5 = out / "hdf5" / f"treedata-{alignment}.h5td"
        zp = out / "zarr" / f"treedata-{alignment}.zarr"
        assert_hdf5(h5, expected)
        assert_zarr(zp, expected)
        assert_roundtrip(expected, read_h5td(h5), str(h5))
        assert_roundtrip(expected, read_zarr(zp), str(zp))
        files.extend(({"path": str(h5), "sha256": hash_path(h5)},
                      {"path": str(zp), "sha256": hash_path(zp)}))
    legacy = out / "zarr" / "treedata-0.2.5-legacy.zarr"
    assert_legacy(legacy, make_treedata("leaves"))
    files.append({"path": str(legacy), "sha256": hash_path(legacy)})
    return {
        "fixtures": files,
        "selection_semantics": assert_selection_semantics(),
        "label_semantics": assert_label_semantics(out),
    }


def inspect_native(path: Path, expected: Any, fmt: str) -> dict[str, str]:
    from treedata import read_h5td, read_zarr
    actual = read_h5td(path) if fmt == "h5td" else read_zarr(path)
    assert_roundtrip(expected, actual, f"native {fmt} output {path}")
    if fmt == "h5td":
        assert_hdf5(path, expected)
    else:
        assert_zarr(path, expected)
    return {"path": str(path), "format": fmt, "sha256": hash_path(path)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="fixture directory")
    parser.add_argument("--mode", choices=("all", "generate", "inspect"), default="all")
    parser.add_argument("--legacy-python", type=Path,
                        default=Path(os.environ["TREEDATA_025_PYTHON"]) if os.environ.get("TREEDATA_025_PYTHON") else None,
                        help="interpreter containing treedata==0.2.5")
    parser.add_argument("--alignment", choices=ALIGNMENTS, help="source alignment for native roundtrip checks")
    parser.add_argument("--roundtrip-h5", type=Path, help="native H5TD output to compare")
    parser.add_argument("--roundtrip-zarr", type=Path, help="native current Zarr output to compare")
    parser.add_argument("--roundtrip-legacy-zarr", type=Path, help="native output from the legacy Zarr fixture")
    parser.add_argument("--_legacy-helper", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    if args._legacy_helper is not None:
        legacy_helper(args._legacy_helper)
        return 0
    if args.out is None:
        parser.error("--out is required unless --_legacy-helper is used")

    prepare_runtime()
    found = require_version("0.4.0")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.mode in ("all", "generate"):
        if args.legacy_python is None:
            parser.error("--legacy-python or TREEDATA_025_PYTHON is required for all/generate")
        if not args.legacy_python.is_file():
            parser.error(f"legacy interpreter not found: {args.legacy_python}")
        write_current(out)
        write_legacy(out / "zarr" / "treedata-0.2.5-legacy.zarr", args.legacy_python)
        if args.mode == "generate":
            print(json.dumps({"versions": found, "generated_under": str(out)}, indent=2, sort_keys=True))
            return 0

    report = inspect(out)
    native = []
    if (args.roundtrip_h5 or args.roundtrip_zarr) and args.alignment is None:
        parser.error("--alignment is required with --roundtrip-h5/--roundtrip-zarr")
    if args.roundtrip_h5:
        native.append(inspect_native(args.roundtrip_h5, make_treedata(args.alignment), "h5td"))
    if args.roundtrip_zarr:
        native.append(inspect_native(args.roundtrip_zarr, make_treedata(args.alignment), "zarr"))
    if args.roundtrip_legacy_zarr:
        native.append(inspect_native(args.roundtrip_legacy_zarr, make_treedata("leaves"), "zarr"))
    report["native_roundtrips"] = native
    report["versions"] = found
    if args.mode != "inspect":
        (out / "treedata-reference-manifest.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

