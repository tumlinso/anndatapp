#!/usr/bin/env python3
"""Run TreeData interoperability checks through the native C++ reader/writer.

The pinned treedata 0.4.0 environment writes current fixtures. Supplying
--legacy-python adds a genuine treedata 0.2.5 scalar-JSON Zarr input.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(binary: Path, *args: str) -> None:
    result = subprocess.run([str(binary), *args], text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"native TreeData command failed (exit {result.returncode}): "
            f"{binary} {args!r}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")


def copy_h5(source: Path, target: Path) -> Path:
    target.parent.mkdir(parents=True, exist_ok=True)
    return Path(shutil.copy2(source, target))


def malformed_h5(source: Path, target: Path) -> Path:
    import h5py
    target = copy_h5(source, target)
    with h5py.File(target, "r+") as store:
        store["obst"][()] = "{invalid-json"
    return target


def overflow_h5(source: Path, target: Path) -> Path:
    import h5py
    target = copy_h5(source, target)
    with h5py.File(target, "r+") as store:
        payload = json.loads(store["obst"].asstr()[()])
        payload["alpha"]["nodes"]["alpha-root"]["overflow"] = 1 << 64
        store["obst"][()] = json.dumps(payload)
    return target


def scalar_query_fixture(source: Path, target: Path, *, hdf5: bool) -> Path:
    """Keep valid JSON but make an unrelated tree's attributes invalid."""
    if hdf5:
        import h5py
        copy_h5(source, target)
        with h5py.File(target, "r+") as store:
            payload = json.loads(store["obst"].asstr()[()])
            payload["beta"]["nodes"]["beta-root"] = 42
            store["obst"][()] = json.dumps(payload)
    else:
        import zarr
        shutil.copytree(source, target)
        store = zarr.open_group(str(target), mode="a")
        payload = json.loads(str(store["obst"][()]))
        payload["beta"]["nodes"]["beta-root"] = 42
        store["obst"][()] = json.dumps(payload)
    return target


def mutate_zarr(source: Path, target: Path, updates: list[tuple[str, int, str]]) -> Path:
    import zarr
    shutil.copytree(source, target)
    group = zarr.open_group(str(target), mode="a")
    for key, index, value in updates:
        group[key][index] = value
    return target


BULK_NODE_COUNT = 5000
LARGE_H5_JSON_BYTES = (1 << 20) + 1
LARGE_ZARR_JSON_BYTES = (1 << 20) + 1


def make_large_json_fixture(reference: object, out: Path, fmt: str) -> tuple[Path, object]:
    from treedata import read_h5td, read_zarr

    expected = reference.make_treedata("leaves")
    size = LARGE_H5_JSON_BYTES if fmt == "h5td" else LARGE_ZARR_JSON_BYTES
    expected.obst["alpha"].nodes["alpha-root"]["large_json"] = {
        "kind": "large",
        "padding": "x" * size,
        "nested": [1, {"value": "tail"}],
    }
    if fmt == "h5td":
        path = out / "hdf5" / "treedata-large-json.h5td"
        path.parent.mkdir(parents=True, exist_ok=True)
        expected.write_h5td(path)
        reference.assert_hdf5(path, expected)
        reference.assert_roundtrip(expected, read_h5td(path), "large H5TD reference fixture")
    else:
        path = out / "zarr" / "treedata-large-json.zarr"
        path.parent.mkdir(parents=True, exist_ok=True)
        expected.write_zarr(str(path))
        reference.assert_zarr(path, expected)
        reference.assert_roundtrip(expected, read_zarr(path), "large Zarr reference fixture")
    return path, expected


def make_bulk_node_fixture(reference: object, out: Path) -> tuple[Path, object]:
    import networkx as nx
    import numpy as np
    import pandas as pd
    import zarr
    from treedata import TreeData, read_zarr

    names = [f"node-{i:05d}" for i in range(BULK_NODE_COUNT)]
    graph = nx.DiGraph()
    graph.add_nodes_from((name, {"rank": i}) for i, name in enumerate(names))
    graph.add_edges_from((names[0], name) for name in names[1:])
    obs = pd.DataFrame(
        {"obs_rank": np.arange(BULK_NODE_COUNT, dtype=np.int32)},
        index=pd.Index(names, name="node_id"),
    )
    var = pd.DataFrame({"var_rank": np.array([0], dtype=np.int16)}, index=["gene-0"])
    expected = TreeData(
        X=np.zeros((BULK_NODE_COUNT, 1), dtype=np.float32),
        obs=obs,
        var=var,
        obst={"bulk": graph},
        label=None,
        alignment="nodes",
        allow_overlap=False,
    )
    path = out / "zarr" / "treedata-bulk-nodes.zarr"
    path.parent.mkdir(parents=True, exist_ok=True)
    expected.write_zarr(str(path))
    actual = read_zarr(path)
    reference.assert_roundtrip(expected, actual, "bulk-node reference fixture")
    stored = zarr.open_group(str(path), mode="r")["obst"]["bulk"]
    assert [str(value) for value in stored["nodes"][:]] == names
    assert stored["node_attrs"]["rank"].dtype.kind in "iu"
    assert stored["node_attrs"]["rank"][:].tolist() == list(range(BULK_NODE_COUNT))
    return path, expected


def roundtrip_bulk_zarr(reference: object, out: Path, binary: Path) -> dict[str, object]:
    from treedata import read_zarr
    import zarr

    source, expected = make_bulk_node_fixture(reference, out)
    target = out / "native" / "treedata-bulk-nodes.zarr"
    target.parent.mkdir(parents=True, exist_ok=True)
    run(binary, "--budget-zarr", str(source), str(target))
    actual = read_zarr(target)
    reference.assert_roundtrip(expected, actual, "native bulk-node Zarr output")
    stored = zarr.open_group(str(target), mode="r")["obst"]["bulk"]
    assert stored["nodes"].shape == (BULK_NODE_COUNT,)
    assert stored["node_attrs"]["rank"][:].tolist() == list(range(BULK_NODE_COUNT))
    return {
        "status": "passed",
        "node_count": BULK_NODE_COUNT,
        "numeric_attribute": "obst/bulk/node_attrs/rank",
        "input_sha256": reference.hash_path(source),
        "output_sha256": reference.hash_path(target),
    }


def reject_tiny_budget(
    reference: object, out: Path, binary: Path, input_h5: Path, input_zarr: Path
) -> dict[str, object]:
    budget_dir = out / "budget"
    budget_dir.mkdir(parents=True, exist_ok=True)
    h5_destination = budget_dir / "preexisting.h5td"
    zarr_destination = budget_dir / "preexisting.zarr"
    shutil.copy2(input_h5, h5_destination)
    shutil.copytree(input_zarr, zarr_destination)
    before_h5 = reference.hash_path(h5_destination)
    before_zarr = reference.hash_path(zarr_destination)
    run(binary, "--reject-budget-h5", str(input_h5), str(h5_destination))
    run(binary, "--reject-budget-zarr", str(input_zarr), str(zarr_destination))
    after_h5 = reference.hash_path(h5_destination)
    after_zarr = reference.hash_path(zarr_destination)
    if after_h5 != before_h5:
        raise AssertionError("H5TD destination changed after tiny-scratch preflight rejection")
    if after_zarr != before_zarr:
        raise AssertionError("Zarr destination changed after tiny-scratch preflight rejection")
    # AnnData sparse packing and validation also precede destructive creation.
    for reason in ("budget", "invalid"):
        run(binary, f"--reject-embedded-{reason}-h5", str(h5_destination))
        run(binary, f"--reject-embedded-{reason}-zarr", str(zarr_destination))
        if reference.hash_path(h5_destination) != before_h5:
            raise AssertionError(f"H5TD destination changed after embedded-{reason} rejection")
        if reference.hash_path(zarr_destination) != before_zarr:
            raise AssertionError(f"Zarr destination changed after embedded-{reason} rejection")
    return {
        "scratch_bytes": 1,
        "h5td_destination_unchanged": True,
        "h5td_sha256": after_h5,
        "zarr_destination_unchanged": True,
        "zarr_sha256": after_zarr,
        "embedded_sparse_capacity_and_validation": "passed; both destinations unchanged",
    }


def reject_large_elements(reference: object, out: Path, binary: Path) -> dict[str, object]:
    cases = []
    for fmt, mode in (("h5td", "--reject-large-h5"), ("zarr", "--reject-large-zarr")):
        source, _ = make_large_json_fixture(reference, out, fmt)
        destination = out / "budget" / ("large-existing.h5td" if fmt == "h5td" else "large-existing.zarr")
        if fmt == "h5td":
            shutil.copy2(source, destination)
        else:
            shutil.copytree(source, destination)
        before = reference.hash_path(destination)
        run(binary, mode, str(source), str(destination))
        after = reference.hash_path(destination)
        if after != before:
            raise AssertionError(f"{fmt} destination changed after large-element preflight rejection")
        cases.append({
            "format": fmt,
            "payload_bytes": LARGE_H5_JSON_BYTES if fmt == "h5td" else LARGE_ZARR_JSON_BYTES,
            "input_sha256": reference.hash_path(source),
            "destination_sha256": after,
            "destination_unchanged": True,
        })
    return {"scratch_bytes": 1 << 20, "cases": cases}


def make_raw(reference: object, out: Path, binary: Path, results: list[dict]) -> None:
    import numpy as np
    import pandas as pd

    raw_var_names = ["raw-gene-a", "raw-gene-b", "raw-gene-c"]
    raw_var = pd.DataFrame(
        {
            "raw_rank": np.array([11, 22, 33], dtype=np.int16),
            "source": ["reference-a", "reference-b", "reference-c"],
        },
        index=pd.Index(raw_var_names, name="raw_gene_id"),
    )
    raw = {
        "X": np.arange(12, dtype=np.float32).reshape(4, 3) + 100,
        "var": raw_var,
    }
    expected = reference.make_treedata("leaves", raw=raw)
    h5_input = out / "hdf5" / "treedata-raw.h5td"
    zarr_input = out / "zarr" / "treedata-raw.zarr"
    h5_output = out / "native" / "treedata-raw.h5td"
    zarr_output = out / "native" / "treedata-raw.zarr"
    h5_input.parent.mkdir(parents=True, exist_ok=True)
    zarr_input.parent.mkdir(parents=True, exist_ok=True)
    h5_output.parent.mkdir(parents=True, exist_ok=True)
    expected.write_h5td(h5_input)
    expected.write_zarr(str(zarr_input))
    for fmt, source, target, mode in (
        ("h5td", h5_input, h5_output, "--roundtrip-h5"),
        ("zarr", zarr_input, zarr_output, "--roundtrip-zarr"),
    ):
        run(binary, mode, str(source), str(target))
        inspected = reference.inspect_native(target, expected, fmt)
        results.append({
            "case": f"raw-{fmt}",
            "input_sha256": reference.hash_path(source),
            "output": inspected,
            "raw_shape": [4, 3],
            "raw_var_names": raw_var_names,
        })


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_treedata_binary", type=Path)
    parser.add_argument(
        "--legacy-python",
        type=Path,
        default=Path(os.environ["TREEDATA_025_PYTHON"]) if os.environ.get("TREEDATA_025_PYTHON") else None,
        help="interpreter containing treedata==0.2.5; enables legacy scalar-JSON case",
    )
    args = parser.parse_args(argv)
    binary = args.test_treedata_binary.resolve()
    if not binary.is_file():
        parser.error(f"test_treedata binary does not exist: {binary}")
    if args.legacy_python is not None and not args.legacy_python.is_file():
        parser.error(f"legacy Python interpreter does not exist: {args.legacy_python}")

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import treedata_reference as reference

    reference.prepare_runtime()
    current_versions = reference.require_version("0.4.0")
    results = []
    error_fixtures = {}
    with tempfile.TemporaryDirectory(prefix="anndatapp-treedata-roundtrip-") as temporary:
        out = Path(temporary)
        reference.write_current(out)

        # Six current-format alignment cases: H5TD and Zarr for each alignment.
        for alignment in reference.ALIGNMENTS:
            expected = reference.make_treedata(alignment)
            input_h5 = out / "hdf5" / f"treedata-{alignment}.h5td"
            output_h5 = out / "native" / f"treedata-{alignment}.h5td"
            output_h5.parent.mkdir(parents=True, exist_ok=True)
            run(binary, "--roundtrip-h5", str(input_h5), str(output_h5))
            results.append({
                "case": f"hdf5-{alignment}",
                "input_sha256": reference.hash_path(input_h5),
                "output": reference.inspect_native(output_h5, expected, "h5td"),
            })

            input_zarr = out / "zarr" / f"treedata-{alignment}.zarr"
            output_zarr = out / "native" / f"treedata-{alignment}.zarr"
            run(binary, "--roundtrip-zarr", str(input_zarr), str(output_zarr))
            results.append({
                "case": f"zarr-{alignment}",
                "input_sha256": reference.hash_path(input_zarr),
                "output": reference.inspect_native(output_zarr, expected, "zarr"),
            })

        # A current AnnData raw has four obs and an independent three-variable
        # axis while TreeData.var contains two variables.
        make_raw(reference, out, binary, results)

        bulk_budget = roundtrip_bulk_zarr(reference, out, binary)
        tiny_budget = reject_tiny_budget(
            reference,
            out,
            binary,
            out / "hdf5" / "treedata-leaves.h5td",
            out / "zarr" / "treedata-leaves.zarr",
        )
        large_element_rejections = reject_large_elements(reference, out, binary)

        scalar_h5 = scalar_query_fixture(
            out / "hdf5" / "treedata-leaves.h5td",
            out / "errors" / "unrelated-tree.h5td", hdf5=True,
        )
        run(binary, "--scalar-query-h5", str(scalar_h5))

        # Current columnar trees may only contain the pinned supported children.
        import zarr
        unknown = out / "zarr" / "unknown-child.zarr"
        shutil.copytree(out / "zarr" / "treedata-leaves.zarr", unknown)
        zarr.open_group(str(unknown), mode="a")["obst"]["alpha"].create_group("unknown")
        run(binary, "--expect-unknown-zarr", str(unknown))

        # Disconnected selections from nodes alignment transition to subset
        # while retaining complete tree topology and node/edge attributes.
        nodes_h5 = out / "hdf5" / "treedata-nodes.h5td"
        nodes_zarr = out / "zarr" / "treedata-nodes.zarr"
        run(binary, "--select-disconnected-h5", str(nodes_h5))
        run(binary, "--select-disconnected", str(nodes_zarr))
        disconnected_selection = {
            "status": "passed",
            "selection": "obs indices 0,2 and var index 0",
            "expected_alignment": "subset",
            "expected_topology": "full alpha and genes trees retained",
        }

        # Rows for alpha-b are discarded by the first-observation selection.
        # Their malformed JSON must not be decoded on that bounded read.
        leaves_zarr = out / "zarr" / "treedata-leaves.zarr"
        bounded = mutate_zarr(
            leaves_zarr,
            out / "zarr" / "treedata-bounded-read.zarr",
            [
                ("obst/alpha/node_attrs/payload", 3, "{invalid-json"),
                ("obst/alpha/edge_attrs/note", 2, "{invalid-json"),
            ],
        )
        run(binary, "--select-only", str(bounded))
        run(binary, "--expect-malformed-zarr", str(bounded))
        bounded_read = {
            "status": "passed",
            "fixture_sha256": reference.hash_path(bounded),
            "poisoned_rows": ["obst/alpha/node_attrs/payload[3]", "obst/alpha/edge_attrs/note[2]"],
            "selected_read": "passed",
            "full_read": "rejected as unsupported_encoding at /obst/alpha/node_attrs/payload",
        }

        # Isolated error fixtures are copies; the canonical fixtures stay intact.
        bad_h5 = malformed_h5(
            out / "hdf5" / "treedata-leaves.h5td",
            out / "errors" / "malformed-obst.h5td",
        )
        run(binary, "--expect-malformed-h5", str(bad_h5))
        overflow_zarr = mutate_zarr(
            leaves_zarr,
            out / "zarr" / "treedata-overflow.zarr",
            [("obst/alpha/node_attrs/payload", 0, str(1 << 64))],
        )
        run(binary, "--expect-unsupported-integer-zarr", str(overflow_zarr))
        large_h5 = overflow_h5(
            out / "hdf5" / "treedata-leaves.h5td",
            out / "errors" / "oversized-integer.h5td",
        )
        run(binary, "--expect-unsupported-integer-h5", str(large_h5))
        error_fixtures = {
            "malformed_h5": {
                "path": str(bad_h5),
                "sha256": reference.hash_path(bad_h5),
                "status": "passed",
                "expected_error": "unsupported_encoding at /obst",
            },
            "oversized_integer_h5": {
                "path": str(large_h5),
                "sha256": reference.hash_path(large_h5),
                "status": "passed",
                "value": str(1 << 64),
                "expected_error": "unsupported_dtype at /obst",
            },
            "oversized_integer_zarr": {
                "path": str(overflow_zarr),
                "sha256": reference.hash_path(overflow_zarr),
                "status": "passed",
                "value": str(1 << 64),
                "expected_error": "unsupported_dtype at /obst/alpha/node_attrs/payload",
            },
        }

        legacy_status = {"status": "skipped", "reason": "--legacy-python was omitted"}
        if args.legacy_python is not None:
            legacy_input = out / "zarr" / "treedata-0.2.5-legacy.zarr"
            reference.write_legacy(legacy_input, args.legacy_python)
            expected = reference.make_treedata("leaves")
            reference.assert_legacy(legacy_input, expected)
            scalar_legacy = scalar_query_fixture(
                legacy_input, out / "zarr" / "legacy-unrelated-tree.zarr", hdf5=False,
            )
            run(binary, "--scalar-query-zarr", str(scalar_legacy))
            legacy_output = out / "native" / "treedata-legacy-current.zarr"
            run(binary, "--legacy-zarr", str(legacy_input), str(legacy_output))
            results.append({
                "case": "zarr-legacy-0.2.5",
                "input_sha256": reference.hash_path(legacy_input),
                "output": reference.inspect_native(legacy_output, expected, "zarr"),
            })
            legacy_status = {
                "status": "passed",
                "writer": "treedata==0.2.5",
                "reader": "native C++",
                "python_reader": "treedata==0.4.0",
            }

    print(json.dumps({
        "binary": str(binary),
        "current_reference_versions": current_versions,
        "current_alignment_cases": 6,
        "raw_axis_cases": 2,
        "bulk_zarr_budget": bulk_budget,
        "disconnected_nodes_selection": disconnected_selection,
        "tiny_budget_preflight": tiny_budget,
        "large_element_rejections": large_element_rejections,
        "bounded_read": bounded_read,
        "scalar_name_and_key_queries": "H5TD passed; legacy Zarr follows legacy status",
        "unknown_columnar_child": "full, selected and key reads rejected",
        "error_fixtures": error_fixtures,
        "legacy": legacy_status,
        "results": results,
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)

