#!/usr/bin/env python3
"""Dense FP16 release cases with separate uninstrumented and diagnostic runs."""

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
from statistics import median
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
SHAPE = (4096, 1024)
BLOCK = (128, 256)
RECTANGLES = tuple((4 + k*251, 9 + (k % 4)*245) for k in range(16))
FORMATS = ("h5-contiguous", "h5-gzip", "zarr2", "zarr3", "zarr3-sharded")
CASES = [("copy", "memory", 32)]
CASES += [("eager-read", fmt, 2) for fmt in FORMATS]
CASES += [("prepared-read", fmt, 4) for fmt in FORMATS]
CASES += [("eager-write", fmt, 1) for fmt in FORMATS if fmt != "zarr3-sharded"]
CASES += [("prepared-write", "zarr3-sharded", 1)]


def suffix(fmt):
    return "h5ad" if fmt.startswith("h5-") else "zarr"


def is_hdf5(fmt):
    return suffix(fmt) == "h5ad"


def fixture(root, fmt, precision):
    return root / f"{fmt}-f{precision}.{suffix(fmt)}"


def expected(precision):
    import numpy as np
    r = np.arange(SHAPE[0], dtype=np.int64)[:, None]
    c = np.arange(SHAPE[1], dtype=np.int64)[None, :]
    x = (((17*r + 13*c + (r*c) % 97) % 1009) - 504) / 16
    return x.astype(f"float{precision}")


def verify(x, oracle):
    import numpy as np
    assert x.dtype == oracle.dtype and x.shape == oracle.shape, (x.dtype, x.shape)
    np.testing.assert_array_equal(x, oracle)


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def tree_identity(path):
    if path.is_file():
        return {"bytes": path.stat().st_size, "sha256": digest(path)}
    h = hashlib.sha256()
    size = 0
    count = 0
    for member in sorted(p for p in path.rglob("*") if p.is_file()):
        count += 1
        size += member.stat().st_size
        h.update(member.relative_to(path).as_posix().encode() + b"\0")
        h.update(bytes.fromhex(digest(member)))
    return {"bytes": size, "files": count, "tree_sha256": h.hexdigest()}


def geometry(fmt, path):
    import h5py
    import zarr
    if is_hdf5(fmt):
        with h5py.File(path) as f:
            x = f["X"]
            return {"chunks": x.chunks, "compression": x.compression, "itemsize": x.dtype.itemsize}
    x = zarr.open_group(path, mode="r", use_consolidated=False)["X"]
    return {"chunks": x.chunks, "shards": x.shards, "compression": [str(c) for c in x.compressors],
            "itemsize": x.dtype.itemsize}


def make_fixtures(root):
    import anndata as ad
    import h5py
    import numcodecs
    import zarr

    root.mkdir(parents=True, exist_ok=False)
    records = {}
    for precision in (16, 32):
        x = expected(precision)
        data = ad.AnnData(X=x)
        for fmt in FORMATS:
            path = fixture(root, fmt, precision)
            if is_hdf5(fmt):
                data.write_h5ad(path)
                if fmt == "h5-gzip":
                    with h5py.File(path, "r+") as f:
                        attrs = dict(f["X"].attrs)
                        del f["X"]
                        f.create_dataset("X", data=x, chunks=(128, 128), compression="gzip",
                                         compression_opts=1).attrs.update(attrs)
                actual = ad.read_h5ad(path).X
            else:
                version = 2 if fmt == "zarr2" else 3
                with ad.settings.override(zarr_write_format=version, auto_shard_zarr_v3=False):
                    data.write_zarr(path, chunks=(128, 128), consolidate_metadata=False)
                store = zarr.open_group(path, mode="a", use_consolidated=False)
                attrs = dict(store["X"].attrs)
                del store["X"]
                options = {"compressors": numcodecs.Zstd(level=3)} if version == 2 else {
                    "compressors": [zarr.codecs.ZstdCodec(level=3)]}
                if fmt == "zarr3-sharded":
                    options["shards"] = (256, 512)
                store.create_array("X", data=x, chunks=(128, 128), **options).attrs.update(attrs)
                actual = ad.read_zarr(path).X
            verify(actual, x)
            records[path.name] = {**tree_identity(path), **geometry(fmt, path)}
    (root / "manifest.json").write_text(json.dumps(records, indent=2) + "\n")


def python_worker(op, fmt, path, passes, negative):
    import anndata as ad
    import h5py
    import numpy as np
    import zarr

    clock = time.perf_counter
    setup_start = clock()
    if op == "eager-read":
        read = ad.read_h5ad if is_hdf5(fmt) else ad.read_zarr
        setup = clock() - setup_start
        start = clock()
        for _ in range(passes):
            observed = read(path)
        runtime = clock() - start
        verify(observed.X, expected(16))
        payload = passes*SHAPE[0]*SHAPE[1]*2
    else:
        writing = op == "prepared-write"
        file = h5py.File(path) if is_hdf5(fmt) else zarr.open_group(path, mode="r+" if writing else "r")
        x = file["X"]
        rectangles = RECTANGLES[:4] if writing else RECTANGLES
        dest = np.empty((len(rectangles), *BLOCK), dtype=np.float16)
        buffers = [dest[k] for k in range(len(rectangles))]
        selectors = [(slice(r, r+BLOCK[0]), slice(c, c+BLOCK[1])) for r, c in rectangles]
        if writing:
            oracle = expected(16)
            for k, selection in enumerate(selectors):
                dest[k] = -oracle[selection] if negative else oracle[selection]

            def transfer(out, selection):
                x.set_basic_selection(selection, out)
        elif is_hdf5(fmt):
            def transfer(out, selection):
                x.read_direct(out, source_sel=selection)
        else:
            wrapped = [zarr.core.buffer.cpu.NDBuffer.from_numpy_array(out) for out in buffers]

            def transfer(out, selection):
                x.get_basic_selection(selection, out=out)

            buffers = wrapped
        setup = clock() - setup_start
        start = clock()
        for _ in range(passes):
            for out, selection in zip(buffers, selectors, strict=True):
                transfer(out, selection)
        runtime = clock() - start
        if is_hdf5(fmt):
            file.close()
        if not writing:
            oracle = expected(16)
            for k, selection in enumerate(selectors):
                verify(dest[k], oracle[selection])
        payload = passes*dest.nbytes
    print(json.dumps({"setup_s": setup, "runtime_s": runtime, "payload_bytes": payload}))


def check_write(fmt, path, precision, negative=False):
    import anndata as ad
    read = ad.read_h5ad if is_hdf5(fmt) else ad.read_zarr
    oracle = expected(precision)
    if negative:
        for r, c in RECTANGLES[:4]:
            oracle[r:r+BLOCK[0], c:c+BLOCK[1]] *= -1
    verify(read(path).X, oracle)
    return {**tree_identity(path), **geometry(fmt, path)}


def child(command, env, timeout):
    if timeout <= 0:
        raise RuntimeError(f"30-second case budget exhausted before {command}")
    result = subprocess.run(command, capture_output=True, text=True, check=False,
                            env=env, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"{command} failed ({result.returncode})\n{result.stdout}\n{result.stderr}")
    return json.loads(result.stdout)


def fingerprint_sources(source):
    # Hash tracked implementation/build inputs and this new driver's source.
    files = subprocess.check_output(
        ["git", "-C", source, "ls-files", "-z", "--cached", "--others", "--exclude-standard",
         "src", "CMakeLists.txt", "cmake"]).split(b"\0")
    paths = {source / os.fsdecode(entry) for entry in files if entry}
    paths.update(ROOT.glob("*.*"))
    paths.update(ROOT.parent / "v1" / file for file in ("alloc_counters.cpp", "io_counters.c",
                                                        "dyld_interpose.h"))
    return {str(path.relative_to(source)): digest(path) for path in sorted(paths)}


def main(args):
    fixtures = args.fixtures.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    inputs = json.loads((fixtures / "manifest.json").read_text())
    for name, recorded in inputs.items():
        actual = tree_identity(fixtures / name)
        if any(recorded[k] != v for k, v in actual.items()):
            raise RuntimeError(f"immutable fixture differs from its manifest: {name}")

    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    environment = dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1",
                       MKL_NUM_THREADS="1", NUMEXPR_NUM_THREADS="1",
                       ZARR_ASYNC_CONCURRENCY="1", ZARR_THREADING_MAX_WORKERS="1")
    environment.pop("LD_PRELOAD", None)
    if environment.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") == "1":
        raise RuntimeError("run measurements with normal asyncio wakeups")
    binary = args.binary.resolve()
    obs = [binary.parent / lib for lib in ("libfp16_alloc_counters.so", "libfp16_io_counters.so")]
    diag_env = dict(environment, LD_PRELOAD=":".join(map(str, obs)))
    source = ROOT.parents[1]
    report = {
        "schema": "anndatapp.fp16-performance.v1",
        "machine": platform.uname()._asdict(), "cpu_affinity": sorted(os.sched_getaffinity(0)),
        "cache_policy": "ordinary OS cache, no eviction; validated inputs are warm",
        "candidate_hdf5": subprocess.check_output(
            ["pkg-config", "--modversion", "hdf5"], text=True).strip(),
        "candidate_compiler": subprocess.check_output(
            ["clang++-18", "--version"], text=True).splitlines()[0],
        "reference_versions": {p: importlib.metadata.version(p) for p in
                               ("anndata", "h5py", "numpy", "zarr")},
        "source_head": subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"],
                                               text=True).strip(),
        "source_sha256": fingerprint_sources(source),
        "bench_sha256": digest(binary),
        "observer_sha256": {p.name: digest(p) for p in obs},
        "fixtures": inputs,
        "cases": {},
    }
    summary = output / "results.json"

    def checkpoint():
        temporary = output / "results.tmp"
        temporary.write_text(json.dumps(report, indent=2) + "\n")
        temporary.replace(summary)

    checkpoint()
    for op, fmt, passes in CASES:
        name = f"{op}-{fmt}"
        if args.only and args.only not in name:
            continue
        writing = op.endswith("-write")
        output_paths = {f"native{p}": output / f"out-{name}-f{p}.{suffix(fmt)}" for p in (16, 32)}
        if op == "prepared-write":
            output_paths["python16"] = output / f"out-{name}-python-f16.{suffix(fmt)}"
            for key, target in output_paths.items():
                shutil.copytree(fixture(fixtures, fmt, int(key[-2:])), target)

        def native(precision, count, observation, timeout, negative):
            path = output_paths[f"native{precision}"] if writing else fixture(fixtures, fmt, precision)
            return child([binary, op, fmt, path, str(precision), str(count),
                          "diag" if observation else "time", "-" if negative else "+"], diag_env if observation else environment,
                         timeout)

        def reference(count, timeout, negative):
            path = output_paths["python16"] if writing else fixture(fixtures, fmt, 16)
            return child([sys.executable, Path(__file__).resolve(), "python-worker",
                          op, fmt, path, str(count), "-" if negative else "+"], environment, timeout)

        use_reference = op in {"prepared-read", "eager-read", "prepared-write"}
        checked = {f"native{p}": native(p, 1, False, 30, True) for p in (16, 32)}
        if use_reference:
            checked["python16"] = reference(1, 30, True)
        payload32 = checked["native32"]["payload_bytes"]
        assert checked["native16"]["payload_bytes"]*2 == payload32
        if use_reference:
            assert checked["python16"]["payload_bytes"] == checked["native16"]["payload_bytes"]
        case = {"passes": passes, "validated": list(checked)}
        if writing:
            case["verified_outputs"] = {
                key: check_write(fmt, path, int(key[-2:]), op == "prepared-write")
                for key, path in output_paths.items()
            }
        report["cases"][name] = case
        checkpoint()
        if args.validate_only:
            print(name, "validated", flush=True)
            continue
        start = time.monotonic()
        def remaining():
            return 30 - (time.monotonic() - start)
        order = ["native16", "native32"]
        if use_reference:
            order.append("python16")
        samples = {key: [] for key in order}
        case["samples"] = samples
        for pair in range(5):
            for key in order if pair % 2 == 0 else reversed(order):
                negative = pair % 2 != 0
                sample = reference(passes, remaining(), negative) if key == "python16" else native(
                    int(key[-2:]), passes, False, remaining(), negative)
                assert sample["payload_bytes"] == passes*checked[key]["payload_bytes"]
                samples[key].append(sample)
                checkpoint()
        case["medians_s"] = {
            key: {field: median(sample[field] for sample in observations)
                  for field in ("runtime_s", "setup_s")}
            for key, observations in samples.items()
        }
        case["diagnostic"] = {f"native{p}": native(p, passes, True, remaining(), True) for p in (16, 32)}
        case["window_s"] = time.monotonic() - start
        if writing:
            case["verified_outputs"] = {
                key: check_write(fmt, path, int(key[-2:]),
                                 op == "prepared-write" and key != "python16")
                for key, path in output_paths.items()
            }
        checkpoint()
        print(name, case["medians_s"], flush=True)
    if report["source_sha256"] != fingerprint_sources(source):
        raise RuntimeError("candidate or benchmark source changed during the campaign")
    print(summary, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("fixtures")
    make.add_argument("root", type=Path)
    run = sub.add_parser("run")
    run.add_argument("--fixtures", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--binary", type=Path, required=True)
    run.add_argument("--cpu", type=int)
    run.add_argument("--validate-only", action="store_true")
    run.add_argument("--only")
    worker = sub.add_parser("python-worker")
    worker.add_argument("op")
    worker.add_argument("fmt")
    worker.add_argument("path", type=Path)
    worker.add_argument("passes", type=int)
    worker.add_argument("sign")
    opts = parser.parse_args()
    if opts.command == "fixtures":
        if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") == "1":
            sys.path.insert(0, str(ROOT.parents[1] / "tests"))
            import python_storage  # existing fixture-only wakeup policy, installed on import
        make_fixtures(opts.root)
    elif opts.command == "python-worker":
        python_worker(opts.op, opts.fmt, opts.path, opts.passes, opts.sign == "-")
    else:
        main(opts)
