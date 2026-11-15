# Bounded installed-prefix comparison

Build preparation and fixture creation happen before any timing window. Root must grant a quiet measurement window after source and installed prefixes are frozen. `window.py` runs a JSON plan serially for at most 30 seconds, including process startup and timeouts. Each plan records file hashes; it takes one initial sample by default and accepts at most three. Preserve incomplete pairs and timeouts. Timed and diagnostic plans must use separate windows and fresh result paths.

## Collection and prepared transfer

The following builds use the same source and Release Clang 18 settings. `DATASET_PREPARED=OFF` compiles against bbe's legacy API; `ON` requires the candidate's new prepared API. Paths are examples from the shared workspace.

```sh
arrow="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow"
for version in bbe candidate; do
  if [ "$version" = bbe ]; then prefix="$PWD/build/format-family-repair/candidate-prefix"; prepared=OFF
  else prefix="$PWD/build/format-family-io/candidate-prefix"; prepared=ON; fi
  cmake -S tools/format-family-screen -B "build/format-family-io/screen/$version" -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
    -DANNDATAPP_ARROW_ROOT="$arrow" -DCMAKE_PREFIX_PATH="$prefix" -DDATASET_PREPARED="$prepared"
  cmake --build "build/format-family-io/screen/$version" --target dataset-screen -j2
done
clang-18 -shared -fPIC -O2 benchmarks/v1/io_counters.c -o build/format-family-io/screen/libv1_io.so -ldl -pthread
clang++-18 -shared -fPIC -O2 benchmarks/v1/alloc_counters.cpp -o build/format-family-io/screen/libv1_alloc.so
```

Generate each immutable plan after builds, preserving any existing plan:

```sh
for mode in timing diagnostic; do
  python3 tools/format-family-screen/make_dataset_plan.py \
    --bbe build/format-family-io/screen/bbe/dataset-screen \
    --candidate build/format-family-io/screen/candidate/dataset-screen \
    --bbe-prefix build/format-family-repair/candidate-prefix \
    --candidate-prefix build/format-family-io/candidate-prefix \
    --fixtures build/format-family-repair/screen/fixtures \
    --io-observer build/format-family-io/screen/libv1_io.so \
    --alloc-observer build/format-family-io/screen/libv1_alloc.so \
    --mode "$mode" --output "build/format-family-io/screen/dataset-$mode-plan.json"
done
```

The plan generator verifies all existing fixture bytes against their stored inventory before writing either plan. Run only after root grants each timing or diagnostic window:

```sh
python3 tools/format-family-screen/window.py --plan build/format-family-io/screen/dataset-timing-plan.json \
  --output build/format-family-io/screen/dataset-timing.jsonl
python3 tools/format-family-screen/window.py --plan build/format-family-io/screen/dataset-diagnostic-plan.json \
  --output build/format-family-io/screen/dataset-diagnostic.jsonl
```

Each selection uses the same 64 evenly distributed rows and genes. The driver independently derives every expected sparse value, index and offset from the fixture formula and rejects mismatches. `end-to-end` measures open, selection, prepare/requirements, allocation, and transfer; open and preparation/allocation are separately reported. `prepared` measures the actual caller-buffer call after setup. Legacy `read_x_into` repeats its internal preparation, while candidate `prepared.read_into` includes its documented automatic preflight; those operation timings are different work and must not be presented as an equivalent-work speed ratio. Compare equivalent-output end-to-end instead. `preflight` is candidate only and is a separate diagnostic operation; it is also included automatically in candidate reads. Counts are scoped by the existing v1 interposers: intercepted libc read/write and H5D calls can omit direct syscalls, internal/hidden calls and mmap page-ins; C++ `new` counts exclude malloc and other allocators. Never use instrumented durations as timing evidence. `getrusage` peak RSS is a process high-water mark, not current retained memory or per-operation allocations; page faults are not physical-byte counters. Normal cache state is uncontrolled.

The core and cross-family plans are described in the associated `core_README.md` and family screen files. A timeout covers startup and setup as well as operation; it is not an operation time. One sample cannot establish variance. Do not use any failed, incomplete or non-golden output in a performance comparison.

## Cross-family comparisons

Build `family-screen` from the same CMake project with the Parquet-enabled bbe and final candidate installed prefixes. Generate and independently verify the small Python reference fixture inventory outside measurement. After `python_family.py`, the fixture inventory, both family binaries and the two v1 observers are available, create one timing and one diagnostic plan per format:

```sh
for mode in timing diagnostic; do
  build/reference/full/bin/python3 tools/format-family-screen/make_family_plans.py \
    --bbe build/format-family-io/screen/family-bbe/family-screen \
    --candidate build/format-family-io/screen/candidate/family-screen \
    --python build/reference/full/bin/python3 \
    --bbe-prefix build/format-family-repair/candidate-prefix \
    --candidate-prefix build/format-family-io/candidate-prefix \
    --fixtures build/format-family-io/fixtures-family-v3 \
    --io-observer build/format-family-io/screen/libv1_io.so \
    --alloc-observer build/format-family-io/screen/libv1_alloc.so \
    --mode "$mode" --output-dir build/format-family-io/screen/family-plans
 done
```

After the root grants a quiet timing window for the format, run e.g. `python3 tools/format-family-screen/window.py --plan build/format-family-io/screen/family-plans/mudata-timing-plan.json --output build/format-family-io/screen/mudata-timing.jsonl`. Each format has a separate 30-second window. Diagnostics use a separate approved window and `*-diagnostic-plan.json`; no diagnostic runtime is a timing sample. Plan generation validates every fixture file against its inventory. All variants must return the same explicitly requested result and independently pass their golden check before timing comparisons. Compare end-to-end requested outputs, not separate phase ratios, when a Python reader eagerly loads data that a backed C++ path acquires lazily; disclose those extra operations. Python import time is reported separately but its process startup still consumes the window. The common matrix and additional domain cases are separate, not aggregate claims about whole formats.

Python timing and diagnostic plans set `ANNDATAPP_INTEROP_ZARR_WAKEUP=0` and use the ordinary event loop. Root runs the bounded window outside the syscall-restricted sandbox. The `=1` test-only policy is used only for fixture generation and readback. If a normal-event-loop process stalls, retain its timeout; do not automatically rerun with another policy.
