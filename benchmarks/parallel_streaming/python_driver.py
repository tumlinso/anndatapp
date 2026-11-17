#!/usr/bin/env python3
"""Persistent Python block readers for the parallel streaming comparison."""

import argparse
from contextlib import contextmanager
from importlib.metadata import version
import json
import multiprocessing
import os
import resource
import sys
import threading
from time import perf_counter_ns
import traceback

SHAPE = (4096, 1024)
SHARD = (256, 512)
INNER_CHUNK = (64, 128)
CSR_ROWS = 64
CSR_PER_ROW = 32
CSR_BLOCKS = SHAPE[0] // CSR_ROWS
ZARR_BLOCKS = (SHAPE[0] // SHARD[0]) * (SHAPE[1] // SHARD[1])
HDF5_CACHE = {"rdcc_nbytes": 1024 * 1024, "rdcc_nslots": 521, "rdcc_w0": 0.75}


@contextmanager
def open_zarr(path, worker, workers):
    import numpy as np
    import zarr
    from zarr.codecs import ZstdCodec
    from zarr.core.buffer.cpu import NDBuffer

    array = zarr.open_group(path, mode="r")["X"]
    try:
        if (array.shape != SHAPE or array.dtype != np.float32
                or array.metadata.zarr_format != 3 or array.chunks != INNER_CHUNK
                or array.shards != SHARD
                or not any(isinstance(codec, ZstdCodec) for codec in array.compressors)):
            raise ValueError("expected the float32 Zstd sharded Zarr v3 streaming fixture")
        output = np.empty(SHARD, dtype=np.float32)
        buffer = NDBuffer.from_numpy_array(output)
        selections = []
        for block in range(worker, ZARR_BLOCKS, workers):
            i, j = divmod(block, SHAPE[1] // SHARD[1])
            r, c = i * SHARD[0], j * SHARD[1]
            selections.append((slice(r, r + SHARD[0]), slice(c, c + SHARD[1])))

        def read(verify):
            for selection in selections:
                array.get_basic_selection(selection, out=buffer)
                if verify:
                    r = np.arange(selection[0].start, selection[0].stop, dtype=np.int64)[:, None]
                    c = np.arange(selection[1].start, selection[1].stop, dtype=np.int64)[None, :]
                    expected = ((17*r + 13*c + r*c % 97) % 1009).astype(np.float32) / np.float32(1009)
                    np.testing.assert_array_equal(output, expected)

        metadata = {
            "versions": {"python": sys.version.split()[0], "numpy": np.__version__, "zarr": zarr.__version__},
            "buffer_bytes_per_worker": output.nbytes,
            "zarr_async_concurrency": zarr.config.get("async.concurrency"),
            "zarr_threading_max_workers": zarr.config.get("threading.max_workers"),
            "zarr_codec_max_workers": zarr.config.get("codec_pipeline.max_workers"),
        }
        yield read, metadata
    finally:
        array.store.close()


@contextmanager
def open_h5ad(path, worker, workers):
    import anndata
    import h5py
    import numpy as np
    import scipy

    with h5py.File(path, "r", **HDF5_CACHE) as handle:
        group = handle["X"]
        dataset = anndata.io.sparse_dataset(group, should_cache_indptr=True)
        if (tuple(dataset.shape) != SHAPE or dataset.format != "csr"
                or any(group[key].dtype != np.int32 or group[key].compression != "gzip"
                       for key in ("data", "indices", "indptr"))):
            raise ValueError("expected the int32 gzip CSR H5AD streaming fixture")
        # Warm the backed reader's own offset cache inside setup.
        _ = dataset._indptr
        selections = [slice(b*CSR_ROWS, (b+1)*CSR_ROWS)
                      for b in range(worker, CSR_BLOCKS, workers)]

        def read(verify):
            for selection in selections:
                block = dataset[selection, :]
                if verify:
                    if (block.format != "csr" or block.shape != (CSR_ROWS, SHAPE[1])
                            or any(buf.dtype != np.int32 for buf in (block.data, block.indices, block.indptr))):
                        raise ValueError("sparse result layout, shape, or width differs")
                    r = np.arange(selection.start, selection.stop, dtype=np.int32)[:, None]
                    k = np.arange(CSR_PER_ROW, dtype=np.int32)
                    cols = np.sort((37*r + 127*k) % SHAPE[1], axis=1)
                    np.testing.assert_array_equal(block.indices, cols.ravel())
                    np.testing.assert_array_equal(block.data, (1 + (11*r + 7*cols) % 11).ravel())
                    np.testing.assert_array_equal(
                        block.indptr, np.arange(CSR_ROWS + 1, dtype=np.int32) * CSR_PER_ROW)
                # The backed Python API constructs one CSR block per read.
                del block

        metadata = {
            "versions": {"python": sys.version.split()[0], "numpy": np.__version__,
                         "anndata": version("anndata"), "h5py": h5py.__version__,
                         "hdf5": h5py.version.hdf5_version, "scipy": scipy.__version__},
            "hdf5_cache": HDF5_CACHE,
            "cached_offset_bytes_per_worker": dataset._indptr.nbytes,
        }
        yield read, metadata


def worker_loop(opener, path, worker, workers, channel):
    try:
        with opener(path, worker, workers) as (read, metadata):
            channel.send(metadata)
            for command in iter(channel.recv, "quit"):
                if command == "verify":
                    read(True)
                    channel.send(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
                elif command == "read":
                    read(False)
                    channel.send(None)
                else:
                    raise ValueError(f"unknown worker command {command!r}")
    except (BrokenPipeError, EOFError):
        return
    except Exception:
        channel.send({"error": traceback.format_exc()})
    finally:
        channel.close()


def command_all(channels, command):
    for channel in channels:
        channel.send(command)
    return receive_all(channels)


def receive_all(channels):
    responses = []
    for channel in channels:
        response = channel.recv()
        if isinstance(response, dict) and "error" in response:
            raise RuntimeError(response["error"])
        responses.append(response)
    return responses


def emit(event, **fields):
    print(json.dumps({"event": event, **fields}, sort_keys=True), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("h5ad", "zarr"))
    parser.add_argument("path")
    parser.add_argument("workers", type=int, choices=(1, 4))
    args = parser.parse_args()
    workers = args.workers
    processes = args.backend == "h5ad"
    opener = open_h5ad if processes else open_zarr

    # Configure the shared Zarr executors before opening any array. HDF5
    # imports and opens happen independently after each process is created.
    start = perf_counter_ns()
    if not processes:
        import zarr

        zarr.config.set({"async.concurrency": workers, "threading.max_workers": workers,
                         "codec_pipeline.max_workers": workers})
    context = multiprocessing.get_context("spawn")
    maker = context.Process if processes else threading.Thread
    channels = []
    active_workers = []

    try:
        for w in range(workers):
            control, reader = context.Pipe()
            child = maker(target=worker_loop, args=(opener, args.path, w, workers, reader), daemon=True)
            child.start()
            channels.append(control)
            active_workers.append(child)
            if processes:
                reader.close()
        metadata = receive_all(channels)[0]
        setup_ns = perf_counter_ns() - start
        verify_start = perf_counter_ns()
        rss = command_all(channels, "verify")
        verification_ns = perf_counter_ns() - verify_start
        blocks = CSR_BLOCKS if processes else ZARR_BLOCKS
        block_bytes = (CSR_ROWS * CSR_PER_ROW * 8 + (CSR_ROWS + 1) * 4 if processes
                       else SHARD[0] * SHARD[1] * 4)
        emit("ready", setup_ns=setup_ns, verification_ns=verification_ns, backend=args.backend,
             workers=workers, blocks=blocks, bytes_per_pass=blocks * block_bytes,
             cpu_affinity=sorted(os.sched_getaffinity(0)), worker_peak_rss_kib=rss,
             worker_model="spawned processes" if processes else "threads", **metadata)
        for line in sys.stdin:
            command = line.strip()
            if command == "quit":
                break
            if command != "read":
                raise ValueError(f"unknown driver command {command!r}")
            begin = perf_counter_ns()
            command_all(channels, "read")
            elapsed_ns = perf_counter_ns() - begin
            emit("sample", elapsed_ns=elapsed_ns)
    finally:
        for channel in channels:
            try:
                channel.send("quit")
            except (BrokenPipeError, EOFError, OSError):
                pass
            channel.close()
        for child in active_workers:
            child.join()
            if processes and child.exitcode != 0:
                raise RuntimeError(f"worker {child.pid} exited with status {child.exitcode}")


if __name__ == "__main__":
    main()
