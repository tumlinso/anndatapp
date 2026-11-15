# Core screen preparation

`core_CMakeLists.txt` compiles the unchanged existing `benchmarks/v1` and `benchmarks/head_to_head` consumers against an installed prefix, and the mmap consumer. These are external consumers, using CMake's module scanning. No benchmark is run by building them. `fixtures-final` below is historical input, outside the new screen fixture budget. Its 303 files matched its manifest on inspection; manifest SHA-256 is `d29c7557a6957e1536628cbaa824f26702fd5c67aa6e0c016d7058bf6345d754` and its recorded generator SHA-256 is `81393ecb1c65bfd565f61566fea24e647c5d89c5c3570034b583ab36507cb7ba`, matching the current generator. Preserve that input: v1 mutates its H5AD and Zarr fixtures.

Build each prefix, substituting an absolute prefix and distinct build directory. Use the matching Parquet-disabled prefixes: `pre-extension-prefix` (e8c), `before-core-prefix` (bbe), and `candidate-core-prefix` (candidate). All three external consumer builds passed against their respective prefixes. The earlier `core-screen-bbe` build targets a different Parquet-enabled prefix and is excluded.

```sh
repo="$PWD"
mkdir -p "$repo/build/format-family-io/core-screen-src"
cp "$repo/tools/format-family-screen/core_CMakeLists.txt" "$repo/build/format-family-io/core-screen-src/CMakeLists.txt"
prefix="$repo/build/format-family-io/pre-extension-prefix"
out="$repo/build/format-family-io/core-screen-pre"
cmake -S "$repo/build/format-family-io/core-screen-src" -B "$out" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$prefix;/home/tumlinson/anndatapp/build/dependencies/usr" \
  -DBENCHMARK_ROOT="$repo/benchmarks" -DCORE_SCREEN_SOURCE="$repo/tools/format-family-screen"
cmake --build "$out"
```

After a quiet timing grant, run identical scenarios against all three binaries using a fresh copy of the historical fixture per invocation; prepare copies outside the window. V1 scenarios `h5ad`, `zarr2`, `zarr3`, `contexts` cover dense H5AD, both CSR/CSC native and opposite selection, Zarr2 dense, Zarr3 gather/write, and reused contexts plus adaptive scheduling. Each process validates against independent formulas; writes are checked by readback. Use `timeout 30s` around the entire source's bounded acquisition and retain raw JSON and exit status; take uninstrumented timing and separately instrumented diagnostic runs. Diagnostics invoke `LD_PRELOAD="$out/libcore_v1_io.so:$out/libcore_v1_alloc.so" "$out/core_v1" SCENARIO COPY --diagnostic`. Their libc counters omit direct syscalls, mmap and hidden symbols; allocations cover the instrumented operator-new paths rather than all allocations. V1 emits transfer time, not independent open/preparation or equivalent end-to-end time. The manual and adaptive schedules may execute different work; do not interpret them as paired identical timings. V1 verifies values independently but does not emit a checksum.

The head-to-head consumer retains its existing SHA-256 check. The runner uses read-only 1024 fixtures from `/home/tumlinson/anndatapp/build/evidence/head-to-head-baseline-20261104/head-to-head/fixtures` for dense H5AD eager reading, CSR rows, CSC columns and Zarr gather. Their recorded manifest and all 80 listed files are checked before the window. The historical generator and current source hashes differ and both are captured. Its preparation interval combines open, selection, preparation and allocation; it does not isolate open time. The consumer emits its measurement before verification; the wrapper sends its acknowledgement immediately and persists both records after process completion. The mmap consumer emits separate open, map, touch, total and SHA-256. Its independently generated contiguous input and expected payload checksum are in `build/format-family-io/screen/mmap-fixture`; V1's dense input is chunked and cannot be mapped. Its byte-wise touch includes page faults and loop overhead and is not equivalent to a bulk transfer.

Six independently verified copies of `fixtures-final` were staged in `build/format-family-io/screen/core-inputs`, one for each version and pass, totaling 19,333,524 apparent bytes. `core_window.py` verifies every listed file before execution, records source and binary hashes, interleaves versions in rotating order by scenario, bounds each pass by a single 30-second deadline, and checkpoints each result. Mmap and the read-only head-to-head cases are included in the timing pass only, following the v1 cases; a 5-second per-process cap and the overall 30-second limit can leave later cases unmeasured. Build the bbe and candidate consumers using `before-core-prefix` / `core-screen-bbe-off` and `candidate-core-prefix` / `core-screen-candidate-off` respectively. Once they are built and a quiet timing window is granted, run:

```sh
python3 tools/format-family-screen/core_window.py timing --output build/format-family-io/screen/core-timing
python3 tools/format-family-screen/core_window.py diagnostic --output build/format-family-io/screen/core-diagnostic
```

Each pass requires a new output directory and its own unmodified input copies. A failed or interrupted pass may have mutated its input; it must not be silently restarted on those copies. The version order and lack of cache conditioning should be reported with results. No timing or validation runs were performed during preparation.
