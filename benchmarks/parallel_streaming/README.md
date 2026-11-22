# Parallel streaming comparison

The drivers read the same two immutable 4096 × 1024 fixtures. They assign blocks round-robin to persistent workers and verify every block against an independent formula before timing. Each timed pass begins at dispatch and ends when all workers finish reading. The setup interval includes creating workers and preparing their handles and buffers. Setup and verification have separate timings. Launching the driver and tearing down its workers are outside the throughput interval. [AnnData++ 1.1.0 results](../results/parallel-streaming-1.1.json)

| Workload | AnnData++ | Python |
| --- | --- | --- |
| Float32 Zstd Zarr v3, 32 complete 256 × 512 shards, 64 × 128 inner chunks | One independent prepared context and reusable 512 KiB output per thread | One independent Zarr array and reusable NumPy `NDBuffer` per caller thread, using `get_basic_selection(..., out=...)` |
| Int32 gzip H5AD CSR, 64 blocks of 64 complete rows and 32 entries per row | One independent HDF5 process with a prepared context and reusable value, index, and offset buffers per worker | One independent HDF5 process per worker using `anndata.io.sparse_dataset`, cached offsets, and one SciPy CSR result allocated per block |

Each one- or four-worker case uses the same allowed physical cores for both implementations. The Python Zarr worker and async limits follow the selected worker count. BLAS, OpenMP and other numerical thread limits are one. HDF5 uses independent handles and a 1 MiB raw chunk-cache setting per reader. The AnnData++ readers retain normal full validation. The HDF5 versions and allocation APIs differ between the two stacks.

Throughput counts useful output bytes. CSR includes the original `int32` values and column indices and the `int32` offset vector of every block. AnnData++ transfer counters come from the untimed verification pass and count library-reported backend calls, logical bytes and staging allocation events. They describe the transfer path and exclude parts of allocator and codec activity. The timings use normal, already warm caches and retain per-worker setup across five alternating pairs.

## Reproduce on Linux

Use the pinned reference environment and a quiet host with normal Python asyncio wakeups. Start with a new ignored evidence directory. Use the established fixture generator, which independently reopens the stores and records their checksums.

```sh
bench_root="$PWD/build/parallel-streaming"
bench_python=/path/to/reference/bin/python3
bench_cpus=2,4,6,8

"$bench_python" benchmarks/streaming_probe/make_fixture.py "$bench_root/fixture"
cmake -S benchmarks/parallel_streaming -B "$bench_root/bin" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DANNDATAPP_SOURCE="$PWD" -DANNDATAPP_BUILD_TESTS=OFF -DANNDATAPP_BUILD_EXAMPLES=OFF
cmake --build "$bench_root/bin" --parallel 2
"$bench_python" benchmarks/parallel_streaming/run.py \
  --cpp "$bench_root/bin/parallel_streaming" --python "$bench_python" \
  --fixtures "$bench_root/fixture" --output "$bench_root/measurements" --cpus "$bench_cpus"
```

Set `ANNDATAPP_DEPENDENCY_PREFIX` during configuration if the compiler needs a private dependency prefix. Choose four distinct available physical cores from one NUMA node. The first supplies the one-worker comparison. A timed window ends within 30 seconds, including driver setup, verification, and cleanup. The runner retains block verification status, the exact inputs, raw samples, and a checkpoint after each accepted case.

For a native regression comparison, build the same C++ driver against both
frozen revisions and add `--baseline-cpp PATH --baseline-source PATH` to the
run command. The runner interleaves the two native drivers and the Python
driver in each of five pairs and reports candidate-to-baseline time ratios.
