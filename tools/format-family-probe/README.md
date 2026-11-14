# AnnData format-family cost probe

This small external consumer checks both supported facade imports and records
bounded caller-facing work. It adds no runtime dependency. Build one mode per
fresh CMake directory so each import spelling gets an independent module build.

## Configure and build

Use the final installed prefix and the exact Clang/scanner pair selected for
qualification. Example:

```sh
cmake -S tools/format-family-probe -B build/cost-probe-prep/import-anndata \
  -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_PREFIX_PATH=/path/to/final-prefix -DPROBE_MODE=anndata
cmake --build build/cost-probe-prep/import-anndata --target format-family-probe

cmake -S tools/format-family-probe -B build/cost-probe-prep/import-anndatapp \
  -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_PREFIX_PATH=/path/to/final-prefix -DPROBE_MODE=anndatapp
cmake --build build/cost-probe-prep/import-anndatapp --target format-family-probe

cmake -S tools/format-family-probe -B build/cost-probe-prep/workload \
  -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_PREFIX_PATH=/path/to/final-prefix -DPROBE_MODE=workload
cmake --build build/cost-probe-prep/workload --target format-family-probe
```

For compiler child-process measurements, set a distinct log for each fresh
build and add
`-DCMAKE_CXX_COMPILER_LAUNCHER="bash;$PWD/tools/format-family-probe/compiler-time-launcher.sh"`
at configure time. Set `ANNDATAPP_PROBE_COMPILER_TIME_LOG` and
`ANNDATAPP_PROBE_COMPILER_LABEL` before building. Each compiler log row includes
the invoked command so consumer translation-unit compilation can be separated
from fresh imported-module BMI compilation. The log does not include CMake's
separate dependency-scanner process.

## Workloads

Run the workload executable after compilation. Fixture creation is separate
from timed reads:

```sh
mkdir -p build/qualification/cost-probe/raw
build/cost-probe-prep/workload/format-family-probe --prepare build/qualification/cost-probe/raw/format-family-fixture.h5
build/cost-probe-prep/workload/format-family-probe --transfer build/qualification/cost-probe/raw/format-family-fixture.h5
build/cost-probe-prep/workload/format-family-probe --transform
```

`--transfer` retains a selected HDF5 array after its `BackedStore` owner leaves
scope. It reads rows `0, 4, ..., 4092` from a contiguous 4096 by 1024 FP32 array
into a caller-owned 2x-strided output. Each of five timed samples transfers
4 MiB of logical values through one reusable context with a 64 KiB scratch
limit. Output values and untouched stride gaps are checked after each sample.
The HDF5 fixture is uncompressed; file-cache warmth is whatever the host gives
the process and should be reported with the samples.

`--transform` applies a fixed 2D affine transform to one million borrowed FP64
coordinate pairs and validates three positions after each of five timed calls.
The caller allocates input and output; only the numerical call is timed.

These are bounded workload observations, not comparative performance claims.
