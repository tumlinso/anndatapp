"""Freeze existing fixtures and compare the same Release consumer across two builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fnv(*arrays):
    digest = 14695981039346656037
    for array in arrays:
        for byte in array.tobytes():
            digest = ((digest ^ byte) * 1099511628211) % (1 << 64)
    return f"{digest:016x}"


def freeze(a):
    import h5py
    import numpy as np

    a.evidence.mkdir(parents=True, exist_ok=True)
    cases = a.evidence / "cases"
    cases.mkdir()
    inputs = set()
    singles = []

    def case(name, file, rows, cols, mode, layer="", gather=(0, 0)):
        file = file.resolve(strict=True)
        inputs.add(file)
        with h5py.File(file) as source:
            x = source["layers/" + layer if layer else "X"]
            if mode.startswith("dense"):
                assert x.dtype == np.dtype("float32")
                digest = fnv(x[:][np.ix_(rows, cols)])
            else:
                layout = x.attrs["encoding-type"]
                assert layout in {"csr_matrix", "csc_matrix"}
                major, minor = (rows, cols) if layout == "csr_matrix" else (cols, rows)
                data, index, offset = (x[k][:] for k in ("data", "indices", "indptr"))
                assert data.dtype == np.dtype("float32")
                selected = {}
                for i, j in enumerate(minor):
                    selected.setdefault(j, []).append(i)
                values, indices, offsets = [], [], [0]
                for j in major:
                    for k in range(int(offset[j]), int(offset[j+1])):
                        for i in selected.get(int(index[k]), ()):
                            values.append(data[k])
                            indices.append(i)
                    offsets.append(len(values))
                digest = fnv(np.array(values, dtype=data.dtype), np.array(indices, dtype=index.dtype),
                             np.array(offsets, dtype=offset.dtype))
        spec = cases / (name + ".txt")
        spec.write_text(f"{name}\n{file}\n{layer}\n{mode}\n{digest}\n{gather[0]} {gather[1]}\n"
                        + f"{len(rows)} {' '.join(map(str, rows))}\n"
                        + f"{len(cols)} {' '.join(map(str, cols))}\n")
        inputs.add(spec.resolve())
        singles.append(str(spec.resolve()))

    dense_rows, dense_cols = range(16, 80, 2), range(32, 96, 2)
    case("dense-gather", a.dense, dense_rows, dense_cols, "dense", gather=(1, 1))
    case("dense-prepared-gather", a.dense, dense_rows, dense_cols, "dense-prepared", gather=(1, 1))
    case("dense-rectangle", a.dense, range(16, 48), range(32, 64), "dense")
    case("csr-gzip-run", a.csr, range(16, 272), range(2048), "sparse")
    case("csc-run", a.csc, range(35), range(5, 65), "sparse", layer="csc")
    case("csr-zero-matches", a.zero, range(8), [1, 3], "sparse", gather=(0, 1))

    manifest = a.collection.resolve(strict=True)
    inputs.update(p.resolve() for p in manifest.parent.glob("*.h5ad"))
    inputs.add(manifest)
    spec = cases / "collection-gather.txt"
    rows = [(193 * i) % 8192 for i in range(64)]
    cols = [(17 * i) % 2048 for i in range(1024)]
    spec.write_text(f"collection-gather\n{manifest}\n8192 2048 16\n64 1024 5\n"
                    + " ".join(map(str, rows)) + "\n" + " ".join(map(str, cols)) + "\n")
    inputs.add(spec.resolve())
    plan = {"inputs": {str(p): sha(p) for p in sorted(inputs)},
            "singles": singles + ["axis-copy"], "collection": str(spec.resolve())}
    (a.evidence / "plan.json").write_text(json.dumps(plan, indent=2) + "\n")


def run(a):
    start = time.monotonic()
    plan = json.loads((a.evidence / "plan.json").read_text())
    variants = {"before": a.before, "after": a.after}
    interposers = [p.resolve(strict=True) for p in a.preload]
    identities = plan["inputs"] | {str(p): sha(p) for p in interposers}
    jobs = [(spec, "hdf5_read_reuse") for spec in plan["singles"]]
    jobs.append((plan["collection"], "collection_read_reuse"))
    for exe in {exe for _, exe in jobs}:
        for path in variants.values():
            exe_path = (path / exe).resolve(strict=True)
            identities[str(exe_path)] = sha(exe_path)

    def verify_inputs():
        for path, expected in identities.items():
            if sha(path) != expected:
                raise ValueError(f"input changed {path}")

    verify_inputs()
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    for key in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
                "NUMEXPR_NUM_THREADS", "NUMBA_NUM_THREADS", "BLOSC_NTHREADS"):
        env[key] = "1"
    records = []
    with (a.evidence / "samples.jsonl").open("x", buffering=1) as receipts:
        for diagnostic, repeats in ((False, a.repeats), (True, 1)):
            for i in range(repeats):
                for spec, exe in jobs:
                    for variant in (("before", "after") if i % 2 == 0 else ("after", "before")):
                        cmd = [str((variants[variant] / exe).resolve()), spec]
                        process_env = env.copy()
                        if diagnostic:
                            cmd.append("--diagnostic")
                            process_env["LD_PRELOAD"] = ":".join(map(str, interposers))
                        remaining = 30 - (time.monotonic() - start)
                        if remaining <= 0:
                            raise TimeoutError("read-reuse campaign reached 30 seconds")
                        result = subprocess.run(cmd, check=True, capture_output=True, text=True,
                                                env=process_env, timeout=remaining)
                        record = json.loads(result.stdout) | {"build": variant, "instrumented": diagnostic}
                        receipts.write(json.dumps(record) + "\n")
                        assert record["golden"] == "pass"
                        records.append(record)
    verify_inputs()
    summary = {"fixture_evidence": str(a.evidence), "elapsed_s": time.monotonic() - start,
               "cpu_affinity": sorted(os.sched_getaffinity(0)), "repeats": a.repeats, "inputs": identities,
               "workloads": {}}
    for case in sorted({r["case"] for r in records}):
        matching = [r for r in records if r["case"] == case]
        for key in ("output_fnv1a64", "nnz", "payload_bytes", "actual_index_dtype", "actual_offset_dtype"):
            if len({r.get(key) for r in matching}) != 1:
                raise ValueError(f"{case}: different {key}")
        stats = {"output_fnv1a64": matching[0]["output_fnv1a64"]}
        for mode in variants:
            timing = [r for r in matching if r["build"] == mode and not r["instrumented"]]
            diag = next(r for r in matching if r["build"] == mode and r["instrumented"])
            fields = ("preparation_ns", "preparation_requirements_ns", "transfer_ns",
                      "operation_ns", "end_to_end_ns")
            phases = {k: statistics.median(r[k] for r in timing) for k in fields if k in timing[0]}
            if "repeat_transfer_ns" in timing[0]:
                phases["repeat_transfer_ns"] = statistics.median(
                    statistics.median(r["repeat_transfer_ns"]) for r in timing)
            stats[mode] = {"median_ns": phases, "diagnostic": diag}
        summary["workloads"][case] = stats
    (a.evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    f = commands.add_parser("freeze")
    f.add_argument("evidence", type=Path)
    for name in ("dense", "csr", "csc", "zero", "collection"):
        f.add_argument("--" + name, type=Path, required=True)
    r = commands.add_parser("run")
    r.add_argument("evidence", type=Path)
    r.add_argument("--before", type=Path, required=True)
    r.add_argument("--after", type=Path, required=True)
    r.add_argument("--preload", type=Path, nargs=3, required=True)
    r.add_argument("--repeats", type=int, default=7)
    a = parser.parse_args()
    if a.command == "run" and a.repeats < 1:
        parser.error("--repeats must be positive")
    (freeze if a.command == "freeze" else run)(a)


if __name__ == "__main__":
    main()
