# Format-family sparse screen

A small external installed-package consumer. Fixtures are deterministic, uncompressed CSR: 10,000 and 100,000 observations by 20,000 variables, 100 stored float32 values per row, split over eight H5ADs and duplicated in a monolithic H5AD. No dense full matrix is created. `generate.py` refuses an existing destination, checks a conservative size estimate against 256 MiB, and records the actual byte count and SHA-256 for each file. Fixture storage in this run is 213,446,160 bytes. Same paths and bytes are used for both libraries.

Build and generate before measurement (the reference Python requires h5py and numpy):

```sh
build/reference/full/bin/python3 tools/format-family-screen/generate.py build/format-family-repair/screen/fixtures
cmake -S tools/format-family-screen -B build/format-family-repair/screen/baseline -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-18 \
  -DANNDATAPP_ARROW_ROOT="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow" \
  -DCMAKE_PREFIX_PATH="$PWD/build/format-family-repair/baseline-prefix"
cmake --build build/format-family-repair/screen/baseline
cmake -S tools/format-family-screen -B build/format-family-repair/screen/candidate -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-18 \
  -DANNDATAPP_ARROW_ROOT="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow" \
  -DCMAKE_PREFIX_PATH="$PWD/build/format-family-repair/candidate-prefix"
cmake --build build/format-family-repair/screen/candidate
```

Do not regenerate an existing fixture. Use a fresh destination if inputs change; respect the combined 256 MiB cap. Preserve the baseline prefix, checkpoint, fixture inventory and raw results. Candidate prefix must be supplied and frozen before its build. No execution of timing cases before the root grants an uncontended measurement window. Execute serially, with normal OS and HDF5 caches (no eviction); order, process startup, competing host activity, file hashes, binary hashes, compiler, case timeouts, correctness and partial results must accompany conclusions. First touch is not a guaranteed cold-cache measurement. The 30-second limit includes process startup and timed-out work, not fixture generation or compilation. At most three samples per case. If time runs out, report incomplete pairs rather than extrapolating.

The consumer uses the same old public API from both installs. `getrusage` peak RSS and minor/major faults cover the process, not necessarily only the measured operation. No interposer is injected in timing; process allocation counts, physical I/O bytes and retained scratch are not measured unless an explicit existing API reports them. HDF5 and kernel page-cache state remain uncontrolled. A zero major-fault delta is not evidence of no I/O.

After the root grants the measurement window, run once:

```sh
python3 tools/format-family-screen/run.py \
  --baseline build/format-family-repair/screen/baseline/format-family-screen \
  --candidate build/format-family-repair/screen/candidate/format-family-screen \
  --baseline-prefix build/format-family-repair/baseline-prefix \
  --candidate-prefix build/format-family-repair/candidate-prefix \
  --fixtures build/format-family-repair/screen/fixtures \
  --output build/format-family-repair/screen/samples.jsonl
```

The runner enforces 30 seconds across all process launches, including setup and timeout time, with a default 3-second cap per case and one sample per case; `--samples` permits at most three if a future separately authorized window is justified. It prioritizes 100k, starts alternate pairs with opposite versions, then tries 10k. A slow setup may prevent a case from producing a sample; the explicit timeouts and partial JSONL are evidence of incomplete coverage, not throughput. Compare checksums, shape and correctness before latency; inspect the partial result for completed pairs and stop for interpretation.

## Raster selectivity diagnostics

The small family fixture also has a direct rectangle control.

~~~sh
screen_build=build/raster-selectivity-2026-11-16/probe-candidate-build
arrow="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow"
cmake -S tools/format-family-screen -B "$screen_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-18 \
  -DANNDATAPP_ARROW_ROOT="$arrow" \
  -DCMAKE_PREFIX_PATH="$PWD/build/raster-selectivity-2026-11-16/candidate-prefix"
cmake --build "$screen_build" --target family-screen -j2
~~~

 Build the raster interposer once, after the v1 observers and the family-screen consumer are ready:

~~~sh
mkdir -p build/raster-selectivity-2026-11-16
cc -shared -fPIC -O2 tools/format-family-screen/raster_observer.c -o build/raster-selectivity-2026-11-16/libraster_observer.so -ldl -lzstd
~~~

After a separate diagnostic window is authorized, invoke each case in its own process:

~~~sh
fixture=build/format-family-io/fixtures-family-v3/spatialdata.zarr
screen=build/raster-selectivity-2026-11-16/probe-candidate-build/family-screen
preload="$PWD/build/raster-selectivity-2026-11-16/libraster_observer.so:$PWD/build/format-family-io/screen/libv1_io.so:$PWD/build/format-family-io/screen/libv1_alloc.so"
LD_PRELOAD="$preload" "$screen" spatialdata-raster "$fixture" --diagnostic
LD_PRELOAD="$preload" "$screen" spatialdata-raster-aligned "$fixture" --diagnostic
~~~

The original raster case keeps its decimated selection: all channels, rows 16:80 by 2, columns 32:96 by 2. The aligned control requests all channels, rows 16:48 by 1, columns 32:64 by 1. Both write 1×32×32 uint16 outputs and check each value against the independent source-coordinate formula. The aligned case reports open, selection, allocation, and transfer counters separately. The interposer emits one stderr record per observed phase, in the same order as the JSON phase objects. Its records include the 16 v1 I/O slots, per-fd reads for the exact image chunk and level metadata paths, and Zstd stream calls, input/output bytes, and frame ends. These are intercepted-call diagnostics, not physical disk traffic; never use instrumented durations as timing evidence. The original case retains one open-through-transfer diagnostic scope.
