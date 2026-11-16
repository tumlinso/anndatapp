# Bounded release 1.1 Python comparison

This installed-consumer probe compares the library at `180c440` with the
existing pinned `build/reference/full/bin/python3` environment. It changes no
production implementation. The completed findings are in
[release11-python-performance.md](../../../docs/release11-python-performance.md).

`cpp_probe.cpp` and `python_probe.py` accept `CASE FIXTURE_PATH` and optional
`--diagnostic`. Cases are `raster-decimated`, `raster-aligned`,
`raster-multichunk` (each also with `-raw`), and `dataset-small`, `dataset-10k`.
Python additionally accepts dataset `-validated` cases for the auxiliary
selected-axis-name check. Python uses simultaneous X indexing and selects
rasters before synchronous computation. Every output is checked elementwise
and receives a canonical FNV-1a digest outside the operation clock; Python also
records SHA-256. The digest does not replace the elementwise oracle.

Build the C++ probe against an immutable Release installed prefix:

```sh
cmake -S tools/format-family-screen/release11 -B EVIDENCE/consumer -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=FROZEN_PREFIX -DANNDATAPP_ARROW_ROOT=PINNED_ARROW
cmake --build EVIDENCE/consumer --parallel 2
clang-18 -shared -fPIC -O2 tools/format-family-screen/release11/io.c \
  -o EVIDENCE/librelease11_io.so -ldl -pthread
clang++-18 -shared -fPIC -O2 benchmarks/v1/alloc_counters.cpp \
  -o EVIDENCE/librelease11_alloc.so
```

Reuse the original small family stores and 10k collection, verified against
their inventories. `fixture.py NEW_DESTINATION` writes only the additional
four-of-sixteen-chunk SpatialData store, refuses existing destinations and
checks its physical shape/chunks/Zstd encoding and 1 MiB limit. Preserve its
printed inventory next to the store. Its final campaign location is
`EVIDENCE/fixtures/multichunk.zarr`. Use persistent storage rather than temporary
directories. The restricted-runner wakeup workaround may be needed for fixture
generation; timing probes explicitly reject it.

`plans.py` verifies original fixture inventories, freezes file/source/binary
hashes and creates three plans. The interpreter argument must retain the
virtual environment's lexical path; do not resolve its symlink to base Python.

```sh
python3 tools/format-family-screen/release11/plans.py EVIDENCE \
  --cpp EVIDENCE/consumer/release11-cpp --prefix FROZEN_PREFIX \
  --python build/reference/full/bin/python3
```

The approved run's persistent `campaign-state.json` records all completed
windows. Do not reset or reuse it. For a separately authorized reproduction,
choose a new evidence directory and initialize a new checkpoint with schema
`anndatapp.release11-performance-state.v1`, the library commit, and `windows`
entries `raster`, `collection`, `diagnostic`, each containing
`{"limit_seconds":30,"status":"not_started"}`. Checkpoint state is private
campaign evidence, not Todo authority.

`campaign.py WINDOW EVIDENCE` fsyncs the running checkpoint before invoking the
existing `window.py` serial runner. It refuses a previously started window;
after interruption, interpret partial receipts rather than automatically
restarting its budget. Each window is at most 30 seconds including startup and
timeouts. Primary timings have three explicitly scheduled observations; raw
storage controls have one. Version order alternates. Diagnostics are separate
and their durations are never speed evidence. All three completed windows total
67.54 seconds. No further measurement is needed to reproduce the report from
these retained receipts.

```sh
python3 tools/format-family-screen/release11/campaign.py raster EVIDENCE
python3 tools/format-family-screen/release11/campaign.py collection EVIDENCE
python3 tools/format-family-screen/release11/campaign.py diagnostic EVIDENCE
python3 tools/format-family-screen/release11/summarize.py EVIDENCE --output NEW_SUMMARY
```

Timing requires the ordinary event loop outside the syscall-restricted runner.
Both sides use one allowed CPU and recorded compute-thread limits; Dask is
synchronous and Zarr I/O concurrency/workers are one. Normal caches are not
evicted. Compare end-to-end requested outputs, not mismatched transfer phases.
The Python selected-name adapter reopens HDF5 resources and is reported beside,
not substituted for, the native API. Native sparse widths are reported.

`io.c` exposes the existing v1 begin/end ABI with only libc slots 0..3 available;
it does not interpose HDF5 or codecs. Path counters identify observed target
chunk reads; they are not physical I/O or decode counters. C++ allocation
interception excludes other allocators and cannot compare total Python/Rust
allocations. `summarize.py` checks every receipt, frozen input and cross-language
digest, verifies one/four touched chunk paths and produces the numeric summary
without launching any workloads.
