#!/usr/bin/env python3
"""Run matched, bounded C++ and Python streaming passes on immutable fixtures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import statistics
import subprocess
from time import monotonic


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WINDOW_SECONDS = 30
PAIRS = 5
CASES = {"zarr": ("dense_zarr3", 32, 16777216),
         "h5ad": ("sparse_csr.h5ad", 64, 1065216)}


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def fixtures(root):
    manifest = json.loads((root / "manifest.json").read_text())
    dense = manifest["fixtures"]["dense_zarr3"]
    csr = manifest["fixtures"]["sparse_csr.h5ad"]
    expected = {f"dense_zarr3/{p}": item["sha256"] for p, item in dense["files"].items()}
    expected["sparse_csr.h5ad"] = csr["sha256"]
    actual = {path.relative_to(root).as_posix() for path in (root / "dense_zarr3").rglob("*")
              if path.is_file()}
    if actual != set(expected) - {"sparse_csr.h5ad"}:
        raise ValueError("fixture contains unexpected or missing Zarr paths")
    for path, digest in expected.items():
        if sha(root / path) != digest:
            raise ValueError(f"fixture bytes changed: {path}")
    return {"manifest_sha256": sha(root / "manifest.json"), "shape": manifest["shape"],
            "zarr_files": len(actual), "zarr_chunks": dense["chunks"], "zarr_shards": dense["shards"],
            "h5ad_compression": csr["compression"], "h5ad_rows_per_block": 64,
            "h5ad_sha256": csr["sha256"], "dense_x_sha256": dense["x_sha256"],
            "csr_values_indices_offsets_sha256": csr["logical_csr_sha256"]}


def timeout_left(deadline):
    remaining = deadline - monotonic()
    if remaining <= 0:
        raise TimeoutError("30-second comparison window exhausted")
    return remaining


class Driver:
    def __init__(self, command, log, env, cpus):
        with log.open("xb") as err:
            self.process = subprocess.Popen(
                ["taskset", "-c", ",".join(map(str, cpus)), *command],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err,
                env=env, start_new_session=True,
            )
        self.pending = bytearray()
        self.log = log

    def request(self, command):
        self.process.stdin.write(command.encode() + b"\n")
        self.process.stdin.flush()

    def receive(self, event, deadline):
        fd = self.process.stdout.fileno()
        while b"\n" not in self.pending:
            if not select.select([fd], [], [], timeout_left(deadline))[0]:
                raise TimeoutError(f"waiting for {event}; see {self.log}")
            block = os.read(fd, 65536)
            if not block:
                raise RuntimeError(f"driver stopped before {event}; see {self.log}")
            self.pending.extend(block)
        line, _, self.pending = self.pending.partition(b"\n")
        observation = json.loads(line)
        if observation.get("event") != event:
            raise ValueError(f"expected {event}, received {observation}")
        return observation

    def wait(self, deadline):
        status = self.process.wait(timeout=timeout_left(deadline))
        if status:
            raise RuntimeError(f"driver failed with status {status}; see {self.log}")

    def stop(self, completed):
        if not completed:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()


def measure(a, env, backend, workers, observations):
    name, blocks, logical_bytes = CASES[backend]
    path = a.fixtures.resolve() / name
    cpus = a.cpus[:workers]
    languages = {"cpp": [str(a.cpp.resolve())], "python": [str(a.python.absolute()), str(HERE / "python_driver.py")]}
    deadline = monotonic() + WINDOW_SECONDS
    start = monotonic()
    drivers, ready, samples = {}, {}, {key: [] for key in languages}
    completed = False

    def record(lang, result):
        observations.write(json.dumps({"backend": backend, "workers": workers, "language": lang, **result}) + "\n")
        observations.flush()

    try:
        for lang, command in languages.items():
            log = a.output / f"{backend}-{workers}-{lang}.stderr"
            driver = Driver([*command, backend, str(path), str(workers)], log, env, cpus)
            drivers[lang] = driver
            item = driver.receive("ready", deadline)
            record(lang, item)
            if (item.get("backend"), item.get("workers"), item.get("blocks"), item.get("bytes_per_pass")) != (
                    backend, workers, blocks, logical_bytes):
                raise ValueError(f"invalid fixture contract from {lang}")
            if min(item["setup_ns"], item["verification_ns"]) <= 0:
                raise ValueError(f"invalid preparation or verification interval from {lang}")
            ready[lang] = item

        for pair in range(PAIRS):
            order = tuple(drivers) if pair % 2 == 0 else tuple(reversed(drivers))
            for lang in order:
                driver = drivers[lang]
                driver.request("read")
                sample = driver.receive("sample", deadline)
                record(lang, {"pair": pair + 1, **sample})
                elapsed = sample["elapsed_ns"]
                if not isinstance(elapsed, int) or elapsed <= 0:
                    raise ValueError(f"invalid sample from {lang}")
                samples[lang].append(elapsed)

        for driver in drivers.values():
            driver.request("quit")
        for driver in drivers.values():
            driver.wait(deadline)
        completed = True
    finally:
        for driver in drivers.values():
            driver.stop(completed)

    medians = {lang: statistics.median(nums) for lang, nums in samples.items()}
    return {"workers": workers, "cpus": cpus, "blocks": blocks, "bytes_per_pass": logical_bytes,
            "window_seconds": monotonic() - start, "median_ns": medians,
            "mib_per_second": {lang: logical_bytes*1e9/(elapsed*1024**2) for lang, elapsed in medians.items()},
            "cpp_to_python_throughput": medians["python"] / medians["cpp"],
            "sample_ranges_ns": {lang: [min(nums), max(nums)] for lang, nums in samples.items()},
            "worker_setup_ns": {lang: item["setup_ns"] for lang, item in ready.items()},
            "verification_ns": {lang: item["verification_ns"] for lang, item in ready.items()},
            "native_runtime": {key: value for key, value in ready["cpp"].items()
                               if key.startswith("verification_") and key != "verification_ns"
                               or key in {"worker_model", "buffer_bytes_per_worker"}},
            "python_runtime": {key: value for key, value in ready["python"].items()
                               if key in {"versions", "worker_model", "hdf5_cache", "cached_offset_bytes_per_worker",
                                          "buffer_bytes_per_worker"}
                               or key.startswith("zarr_")}}


def cpu_ids(arg):
    cpus = [int(token) for token in arg.split(",")]
    if len(cpus) != 4 or len(set(cpus)) != 4 or not set(cpus) <= os.sched_getaffinity(0):
        raise ValueError("select four distinct available CPU IDs")
    topology = {}
    for line in subprocess.check_output(["lscpu", "-p=CPU,CORE,NODE"], text=True).splitlines():
        if not line.startswith("#"):
            cpu, core, node = map(int, line.split(","))
            topology[cpu] = (core, node)
    if len({topology[cpu][0] for cpu in cpus}) != 4 or len({topology[cpu][1] for cpu in cpus}) != 1:
        raise ValueError("select four physical cores on one NUMA node")
    return cpus


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cpp", type=Path, required=True)
    p.add_argument("--python", type=Path, required=True)
    p.add_argument("--fixtures", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cpus", type=cpu_ids, required=True, help="four distinct allowed physical core IDs")
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    fixture_identity = fixtures(a.fixtures)
    env = os.environ.copy()
    env.pop("ANNDATAPP_INTEROP_ZARR_WAKEUP", None)
    env.pop("ANNDATA_INTEROP_ZARR_WAKEUP", None)
    env.pop("LD_PRELOAD", None)
    thread_limits = {name: "1" for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
                                             "NUMEXPR_NUM_THREADS", "BLOSC_NTHREADS", "NUMBA_NUM_THREADS")}
    env.update(thread_limits)
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    implementation = [ROOT / "CMakeLists.txt", *sorted((ROOT / "src").rglob("*")),
                      *sorted((ROOT / "cmake").glob("*.cmake*"))]
    inputs = {path.relative_to(ROOT).as_posix(): sha(path) for path in implementation if path.is_file()}
    (a.output / "source-sha256.json").write_text(json.dumps(inputs, indent=2) + "\n")
    cache = a.cpp.parent / "CMakeCache.txt"
    config = {}
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_FLAGS_RELEASE:", "ANNDATAPP_HDF5_VERSION:",
                                "ANNDATAPP_ZSTD_VERSION:")):
                key, value = line.split("=", 1)
                config[key.split(":", 1)[0]] = value
        config["cache_sha256"] = sha(cache)
    info = subprocess.check_output(["lscpu", "--json"], text=True)
    (a.output / "cpu.json").write_text(info)
    host = {item["field"].strip(":"): item["data"] for item in json.loads(info)["lscpu"]}
    manifest = {"fixture_identity": fixture_identity, "max_seconds_per_window": WINDOW_SECONDS,
                "pairs": PAIRS, "host": {key: host[key] for key in ("Architecture", "Model name")},
                "thread_limits": thread_limits, "native_build": config, "case_results": {}}
    manifest["source_inputs_sha256"] = sha(a.output / "source-sha256.json")
    manifest["driver_sha256"] = {"cpp_binary": sha(a.cpp), "cpp_source": sha(HERE / "cpp_driver.cpp"),
                                 "python_source": sha(HERE / "python_driver.py"), "runner": sha(Path(__file__))}
    with (a.output / "observations.jsonl").open("x", buffering=1) as observations:
        for backend in CASES:
            for workers in (1, 4):
                key = f"{backend}-{workers}"
                manifest["case_results"][key] = measure(a, env, backend, workers, observations)
                (a.output / "checkpoint.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if fixtures(a.fixtures) != fixture_identity or any(sha(ROOT / path) != value for path, value in inputs.items()):
        raise ValueError("fixture or implementation inputs changed during the campaign")
    manifest["observations_sha256"] = sha(a.output / "observations.jsonl")
    (a.output / "summary.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
