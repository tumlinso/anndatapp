# Collection execution resource comparison

This paired installed-consumer screen compares the current metadata-reuse
baseline (`d479d5d`, archive SHA-256 `91aa9e1ea7fa4fad36f945e715255ceb7d5f69cab55a179e7f46ae9c77948b46`)
with the execution-resource candidate and native SnapATAC2 2.10.0. Both C++
versions use Release, the same compiled probe source, full structural/numeric
validation, and default positional axis policy. The candidate and baseline
native C++ output widths must match; Python native widths are recorded.
No biological stores are generated. The fixture and selector identities match
the earlier independently checked collection matrix.

Four primary cases—32 components, interleaved reverse/duplicate selections,
and two open-handle reuse workloads—each have three fresh-process observations
per version. Each primary timing process measures first open through materialized
output and four repeated reads of the same selection. The C++ repeats reuse the plan
and caller buffers; Python repeats native indexing, including allocation. These
are API lifecycle comparisons, not allocation-equivalent kernels. One-component,
contiguous and gzip controls have one observation each. Every output is checked
against the independent source formula and canonical digest outside clocks.
Native CSR ordering may differ. Diagnostics are separate instrumented C++
processes, each with a first read and three repeats, and do not provide
elapsed-time speed evidence.

The two windows contain 28 and 25 processes, respectively. Each is capped at
30 seconds **including runner startup and input hashing**. The campaign flushes
a checkpoint before launching, places the runner/children in a process group,
kills it at the cap, stops after the first failed process, and refuses to retry a started window. Record any partial
receipts and failures. The second window should follow only if the first screen
supports continued qualification. Pin both windows to the same allowed CPU and
keep competing build/measurement workloads quiet. Caches remain normal and
are not evicted.

From this repository, `setup.py` copies the already qualified baseline prefix,
existing probes and interposers to the ignored evidence directory and hashes
them. It refuses to overwrite a previous experiment. Before source changes,
create a fresh evidence directory and archive the source commit; configure and
build both consumers against the copied baseline, then seal their hashes.
After the candidate release build has been validated, configure the same
consumers against its prefix with identical compiler and dependency settings:

```sh
E="$PWD/build/collection-execution-fix-2026-11-17"
A="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow"
BENCH_CPU=2  # Choose an allowed, quiet CPU on the host.
mkdir -p "$E"
git archive --format=tar 5d21232 > "$E/baseline-source.tar"
python3 tools/format-family-screen/collection-execution-fix/setup.py
for mode in baseline candidate; do
  cmake -S "$E/source/collection-execution-fix" -B "$E/$mode-consumer" -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$E/$mode-prefix" \
    -DArrowParquet_INCLUDE_DIR="$A/include" -DArrowParquet_ARROW_LIBRARY="$A/libarrow.so.2100" \
    -DArrowParquet_PARQUET_LIBRARY="$A/libparquet.so.2100" > "$E/$mode-configure.log" 2>&1
  cmake --build "$E/$mode-consumer" --parallel 2 > "$E/$mode-build.log" 2>&1
  ninja -C "$E/$mode-consumer" -t commands execution-time | tail -1 > "$E/$mode-link.txt"
  if [ "$mode" = baseline ]; then
    python3 tools/format-family-screen/collection-execution-fix/seal_baseline.py
  fi
done
python3 tools/format-family-screen/collection-execution-fix/freeze.py --candidate-prefix "$E/candidate-prefix"
taskset -c "$BENCH_CPU" python3 tools/format-family-screen/collection-execution-fix/campaign.py screen "$E"
python3 tools/format-family-screen/collection-execution-fix/inspect_screen.py "$E"
```

The candidate prefix must already exist before the candidate build in the loop.
The evidence and reference locations are fixed by the scripts to preserve this
campaign's provenance. Never rerun these setup commands in an existing evidence
directory; use a separately authorized campaign and a fresh destination.

Review `screen-summary.json`, the raw receipt, and any correctness or material
regression flags before deciding whether to spend the second window. If the
screen justifies continuing and the host is quiet, use the same CPU:

```sh
E="$PWD/build/collection-execution-fix-2026-11-17"
BENCH_CPU=2
taskset -c "$BENCH_CPU" python3 tools/format-family-screen/collection-execution-fix/campaign.py repeat "$E"
python3 tools/format-family-screen/collection-execution-fix/summarize.py "$E"
```

The single-sample controls screen regressions but cannot establish stable
latency. A material flag requires both an absolute change greater than 0.1 ms
and a relative change greater than 10%. Three observations do not establish
statistical certainty. Interposed C++ `new` excludes other allocators;
intercepted libc reads/bytes include metadata and read-ahead and are not
physical device I/O, mmap page-ins or live/peak memory. H5Dopen2 may be zero
because the backend opens datasets through H5Oopen.
