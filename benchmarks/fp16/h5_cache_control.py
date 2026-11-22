#!/usr/bin/env python3
"""Matched and default h5py cache controls for the prepared gzip H5AD case."""

import argparse
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
from statistics import median
import sys
import time

import run as bench


def python_sample(path, matched):
    # Pre-import the reference libraries as in the untimed import scope of run.py.
    import anndata
    import h5py

    original_file = h5py.File

    class MatchedFile(original_file):
        def __init__(self, *args, **options):
            super().__init__(*args, rdcc_nbytes=1024*1024, rdcc_nslots=521,
                             rdcc_w0=0.75, **options)

    if matched:
        h5py.File = MatchedFile
    try:
        stream = io.StringIO()
        with redirect_stdout(stream):
            bench.python_worker("prepared-read", "h5-gzip", path, 4, False)
        with h5py.File(path) as store:
            cache = store["X"].id.get_access_plist().get_chunk_cache()
    finally:
        h5py.File = original_file
    result = json.loads(stream.getvalue())
    result["raw_chunk_cache"] = dict(zip(("slots", "bytes", "preemption"), cache, strict=True))
    result["hdf5"] = h5py.h5.get_libversion()
    print(json.dumps(result))


def campaign(args):
    os.sched_setaffinity(0, {args.cpu})
    path = args.fixture.resolve()
    binary = args.binary.resolve()
    environment = dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1",
                       MKL_NUM_THREADS="1", NUMEXPR_NUM_THREADS="1")
    environment.pop("LD_PRELOAD", None)
    assert environment.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") != "1"
    cases = {
        "native16": [binary, "prepared-read", "h5-gzip", path, "16", "4", "time", "+"],
        "python16_default": [sys.executable, __file__, "worker", path, "default"],
        "python16_matched": [sys.executable, __file__, "worker", path, "matched"],
    }
    receipt = {
        "case": "prepared-read-h5-gzip",
        "payload_bytes": 4194304,
        "cpu": args.cpu,
        "benchmark_sha256": bench.digest(binary),
        "driver_sha256": bench.digest(Path(__file__)),
        "shared_runner_sha256": bench.digest(Path(bench.__file__)),
        "fixture": bench.tree_identity(path),
        "native_raw_chunk_cache": {"slots": 521, "bytes": 1048576, "preemption": 0.75},
        "samples": {case: [] for case in cases},
    }
    start = time.monotonic()
    for pair in range(5):
        for case in cases if pair % 2 == 0 else reversed(cases):
            sample = bench.child(cases[case], environment, 30 - (time.monotonic() - start))
            assert sample["payload_bytes"] == receipt["payload_bytes"]
            receipt["samples"][case].append(sample)
    receipt["window_s"] = time.monotonic() - start
    receipt["medians_s"] = {
        case: {field: median(s[field] for s in samples) for field in ("runtime_s", "setup_s")}
        for case, samples in receipt["samples"].items()
    }
    with args.output.open("x") as f:
        json.dump(receipt, f, indent=2)
        f.write("\n")
    print(json.dumps(receipt["medians_s"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--cpu", type=int, required=True)
    run.add_argument("--fixture", type=Path, required=True)
    run.add_argument("--binary", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    worker = sub.add_parser("worker")
    worker.add_argument("path", type=Path)
    worker.add_argument("cache", choices=("matched", "default"))
    args = parser.parse_args()
    if args.command == "worker":
        python_sample(args.path, args.cache == "matched")
    else:
        campaign(args)
