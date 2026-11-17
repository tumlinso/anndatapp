#!/usr/bin/env python3
"""Read one frozen AnnDataSet selection with SnapATAC2 and verify its CSR output."""

import argparse
from collections import defaultdict
import importlib.metadata
import json
from pathlib import Path
import sys
import time


def fnv1a64(payload):
    value = 14695981039346656037
    for byte in memoryview(payload).cast("B"):
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def resolve_manifest(value, case_path):
    manifest = Path(value).expanduser()
    if not manifest.is_absolute():
        manifest = case_path.parent / manifest
    return manifest.resolve()


def expected_csr(rows, cols, nnz_per_row, step, row_modulus, value_modulus, np):
    output_columns = defaultdict(list)
    for output_column, source_column in enumerate(cols):
        output_columns[int(source_column)].append(output_column)

    values = []
    indices = []
    indptr = [0]
    for source_row in rows:
        row_pairs = []
        row = int(source_row)
        base = row % row_modulus
        for k in range(nnz_per_row):
            source_column = step * k + base
            value = np.float32(1 + ((row + k) % value_modulus))
            row_pairs.extend((position, value) for position in output_columns.get(source_column, ()))
        row_pairs.sort(key=lambda pair: pair[0])
        indices.extend(position for position, _value in row_pairs)
        values.extend(value for _position, value in row_pairs)
        indptr.append(len(values))
    return (
        np.asarray(values, dtype=np.float32),
        np.asarray(indices, dtype=np.int64),
        np.asarray(indptr, dtype=np.int64),
    )


def validate_csr(selected, rows, cols, formula, np):
    expected_shape = (len(rows), len(cols))
    if selected.format != "csr" or selected.shape != expected_shape:
        raise ValueError(f"expected CSR shape {expected_shape}, got {selected.format} {selected.shape}")
    if selected.data.dtype != np.dtype("float32"):
        raise ValueError(f"expected float32 values, got {selected.data.dtype}")
    if selected.indptr.shape != (len(rows) + 1,):
        raise ValueError("CSR row-pointer length does not match output rows")
    if int(selected.indptr[0]) != 0 or int(selected.indptr[-1]) != selected.data.size:
        raise ValueError("CSR row pointers do not span the output values")
    if selected.indices.size != selected.data.size or np.any(selected.indices < 0) or np.any(selected.indices >= len(cols)):
        raise ValueError("CSR output indices are inconsistent or out of bounds")
    for row in range(len(rows)):
        first, last = int(selected.indptr[row]), int(selected.indptr[row + 1])
        if last < first:
            raise ValueError("CSR row pointers are not monotone")
        if np.unique(selected.indices[first:last]).size != last - first:
            raise ValueError(f"duplicate output column in CSR row {row}")

    expected_values, expected_indices, expected_indptr = expected_csr(
        rows,
        cols,
        formula["nnz_per_row"],
        formula["column_step"],
        formula["row_modulus"],
        formula["value_modulus"],
        np,
    )
    canonical = selected.copy()
    canonical.sort_indices()
    if not np.array_equal(canonical.indptr.astype(np.int64, copy=False), expected_indptr):
        raise ValueError("CSR row offsets differ from the independent formula oracle")
    if not np.array_equal(canonical.indices.astype(np.int64, copy=False), expected_indices):
        raise ValueError("CSR selected columns differ from the independent formula oracle")
    if not np.array_equal(canonical.data.astype(np.float32, copy=False), expected_values):
        raise ValueError("CSR selected values differ from the independent formula oracle")

    canonical_bytes = (
        np.asarray(canonical.data, dtype="<f4").tobytes(order="C")
        + np.asarray(canonical.indices, dtype="<u4").tobytes(order="C")
        + np.asarray(canonical.indptr, dtype="<u8").tobytes(order="C")
    )
    return {
        "shape": list(canonical.shape),
        "nnz": int(canonical.nnz),
        "native_dtypes": {
            "data": str(selected.data.dtype),
            "indices": str(selected.indices.dtype),
            "indptr": str(selected.indptr.dtype),
        },
        "canonical_dtypes": {"data": "float32", "indices": "uint32", "indptr": "uint64"},
        "canonical_fnv1a64": fnv1a64(canonical_bytes),
        "golden": "pass",
        "selection_semantics": "simultaneous X[rows, cols]; output rows and columns retain selector order and duplicate positions",
    }


def load_formula(case, nnz_per_row):
    supplied = case.get("formula", {})
    default_steps = {16: 127, 128: 15}
    step = int(supplied.get("column_step", case.get("column_step", default_steps.get(nnz_per_row, 0))))
    if step <= 0:
        raise ValueError("case JSON must provide formula.column_step for this nnz_per_row")
    return {
        "nnz_per_row": nnz_per_row,
        "column_step": step,
        "row_modulus": int(supplied.get("row_modulus", case.get("row_modulus", 97))),
        "value_modulus": int(supplied.get("value_modulus", case.get("value_modulus", 31))),
    }


def selector_argument(positions, source_length, np):
    """Use a compact native selector for all/range vectors, retaining positions for the oracle."""
    if positions.size == source_length and np.array_equal(positions, np.arange(source_length, dtype=np.int64)):
        return slice(None), "all", 1
    if positions.size == 1:
        return slice(int(positions[0]), int(positions[0]) + 1), "range", 1
    if positions.size > 1:
        differences = positions[1:] - positions[:-1]
        if differences[0] > 0 and np.all(differences == differences[0]):
            step = int(differences[0])
            return slice(int(positions[0]), int(positions[-1]) + 1, step), "range", step
    return positions, "indices", None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case_json", type=Path)
    parser.add_argument("--repeat", type=int, help="override the repeat count frozen in the case JSON")
    parser.add_argument("--verify-only", action="store_true", help="check support and exact CSR semantics without latency fields")
    args = parser.parse_args()
    case_path = args.case_json.resolve()
    case = json.loads(case_path.read_text())
    name = str(case["name"])
    source_shape = tuple(int(value) for value in case.get("shape", (8192, 2048)))
    if len(source_shape) != 2 or any(value <= 0 for value in source_shape):
        raise ValueError("shape must contain positive source row and column counts")
    rows = [int(value) for value in case["rows"]]
    cols = [int(value) for value in case["cols"]]
    if not rows or not cols:
        raise ValueError("rows and cols selectors must be non-empty")
    if min(rows) < 0 or max(rows) >= source_shape[0] or min(cols) < 0 or max(cols) >= source_shape[1]:
        raise ValueError("selector position lies outside the source matrix")
    nnz_per_row = int(case["nnz_per_row"])
    formula = load_formula(case, nnz_per_row)
    repeat = args.repeat if args.repeat is not None else int(case.get("repeat", 1))
    if repeat < 1:
        raise ValueError("repeat must be at least one")
    manifest = resolve_manifest(case["manifest"], case_path)

    imports_start = time.perf_counter_ns() if not args.verify_only else None
    import numpy as np
    import snapatac2 as snap
    imports_ns = time.perf_counter_ns() - imports_start if imports_start is not None else None
    if snap.__version__ != "2.10.0":
        raise RuntimeError(f"expected pinned SnapATAC2 2.10.0, found {snap.__version__}")
    row_positions = np.asarray(rows, dtype=np.int64)
    col_positions = np.asarray(cols, dtype=np.int64)
    row_selector, row_selector_kind, row_selector_step = selector_argument(row_positions, source_shape[0], np)
    col_selector, col_selector_kind, col_selector_step = selector_argument(col_positions, source_shape[1], np)

    timings = not args.verify_only
    open_start = time.perf_counter_ns() if timings else None
    dataset = snap.read_dataset(manifest, mode="r")
    open_end = time.perf_counter_ns() if timings else None
    operation_samples_ns = []
    first_operation_end = None
    first_output = None
    final_output = None
    try:
        for iteration in range(repeat):
            operation_start = time.perf_counter_ns() if timings else None
            try:
                selected = dataset.X[row_selector, col_selector]
            except Exception as error:
                receipt = {
                    "case": name,
                    "fixture": str(manifest),
                    "status": "reader_capability_failure",
                    "error_type": type(error).__name__,
                    "error": str(error),
                    "selection": {"rows": rows, "cols": cols},
                    "selection_semantics_requested": "simultaneous X[rows, cols] preserving selector order and duplicate positions",
                    "row_selector_kind": row_selector_kind,
                    "row_selector_step": row_selector_step,
                    "col_selector_kind": col_selector_kind,
                    "col_selector_step": col_selector_step,
                    "measurement": "omitted" if args.verify_only else "partial; no successful operation sample",
                }
                if not args.verify_only:
                    receipt.update({
                        "open_ns": open_end - open_start,
                        "python_imports_ns": imports_ns,
                    })
                print(json.dumps(receipt, sort_keys=True, separators=(",", ":")))
                return 2
            operation_end = time.perf_counter_ns() if timings else None
            if timings:
                operation_samples_ns.append(operation_end - operation_start)
                if iteration == 0:
                    first_operation_end = operation_end
            current_output = validate_csr(selected, row_positions, col_positions, formula, np)
            if first_output is None:
                first_output = current_output
            elif current_output["canonical_fnv1a64"] != first_output["canonical_fnv1a64"]:
                raise ValueError(f"repeated operation {iteration + 1} changed the canonical CSR result")
            final_output = current_output
    finally:
        dataset.close()

    receipt = {
        "case": name,
        "fixture": str(manifest),
        "status": "pass",
        "golden": "pass",
        "shape": final_output["shape"],
        "nnz": final_output["nnz"],
        "output_fnv1a64": final_output["canonical_fnv1a64"],
        "value_dtype": final_output["native_dtypes"]["data"],
        "actual_index_dtype": final_output["native_dtypes"]["indices"],
        "actual_offset_dtype": final_output["native_dtypes"]["indptr"],
        "selection": {"rows": rows, "cols": cols},
        "row_selector_kind": row_selector_kind,
        "row_selector_step": row_selector_step,
        "col_selector_kind": col_selector_kind,
        "col_selector_step": col_selector_step,
        "selection_semantics": final_output["selection_semantics"],
        "output": final_output,
        "repeat_count": repeat,
        "measurement": "verify_only_no_latency" if args.verify_only else "uninstrumented_single_process",
        "versions": {
            "snapatac2": snap.__version__,
            "numpy": np.__version__,
            "scipy": importlib.metadata.version("scipy"),
            "h5py": importlib.metadata.version("h5py"),
        },
    }
    if timings:
        receipt.update({
            "python_imports_ns": imports_ns,
            "open_ns": open_end - open_start,
            "operation_ns": operation_samples_ns[0],
            "repeat_transfer_ns": operation_samples_ns[1:],
            "end_to_end_ns": first_operation_end - open_start,
            "first_operation_ns": operation_samples_ns[0],
            "repeat_ns": operation_samples_ns[1:],
            "operation_samples_ns": operation_samples_ns,
            "total_end_to_end_ns": first_operation_end - open_start,
            "total_end_to_end_scope": "native manifest open start through first materialized selector result; excludes imports, oracle validation, repeats and close",
            "final_reused_output_fnv1a64": final_output["canonical_fnv1a64"],
        })
    print(json.dumps(receipt, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
