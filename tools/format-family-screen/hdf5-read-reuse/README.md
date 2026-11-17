# HDF5 backed read reuse comparison

The same installed C++ consumer runs against two Release builds with the same
compiler and dependencies. `run.py freeze` takes existing fixtures and records
their hashes and the selected output oracles. The single-file HDF5 oracles use
h5py and compare the exact caller-buffer bytes, including sparse order, native
widths, duplicate entries and explicit zeros. The collection probe checks
each output against the collection fixture's generating formula. The
metadata-only case checks every copied observation and variable name.

The selected inputs are a 256 × 256 float32 dense H5AD with 128 × 128 gzip chunks,
part 000 of the existing eight-component gzip CSR collection (1024 × 2048),
the `csc` layer from the existing 35 × 67 chunked interoperability fixture,
the 8 × 16 CSR review fixture with two entries at column 2 per row, and the
existing 8192 × 2048 eight-component CSR collection with 16 entries per row.
Input hashes and selectors are in the raw plan. The wide collection gather
measures execution with a retained plan and preallocated output.

Install each build into its own prefix, and configure this probe twice with
the same Clang 18 and Release options. Set `CMAKE_PREFIX_PATH` to the applicable
install prefix and the storage dependency prefix. Build `hdf5_read_reuse`
and `collection_read_reuse`. Compile the existing diagnostic interposers
`../collection-fix/metadata.c`, `../collection-preparation/io.c` and
`../collection-preparation/alloc.cpp` as shared objects.

```sh
python tools/format-family-screen/hdf5-read-reuse/run.py freeze "$EVIDENCE" \
  --dense "$DENSE" --csr "$CSR" --csc "$CSC" --zero "$ZERO" --collection "$COLLECTION"
timeout 30s taskset -c "$BENCH_CPU" python tools/format-family-screen/hdf5-read-reuse/run.py run "$EVIDENCE" \
  --before "$BEFORE_CONSUMER" --after "$AFTER_CONSUMER" \
  --preload "$METADATA_SO" "$IO_SO" "$ALLOC_SO"
```

The seven fresh-process timing pairs per workload alternate variant order
on one CPU with normal caches and no eviction. Single-file clocks cover
prepared requirements when used, and transfer into existing buffers. The
collection consumer also reports first-read end-to-end time and repeated
transfer time. Independent verification is outside the reported clocks.

Each variant also runs once per workload with instrumentation in a separate
process. HDF5 calls count API requests. Intercepted reads include backend
read-ahead and metadata. The requested allocation bytes count only observed
C++ allocations and are cumulative. None of these counters measures disk
traffic or peak memory. Raw samples, identities, and the plan stay in the
local evidence directory.
