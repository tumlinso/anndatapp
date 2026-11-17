# Bounded collection matrix

This installed-consumer suite expands the previous two-case collection screen
without changing production code. It uses the frozen Release installation from
optional axis-name validation (`32852b6`), the existing SnapATAC2 2.10.0 reference
environment and common explicitly recorded row/column position vectors.
SnapATAC2 executes its native Rust-backed `X[rows, cols]` API.

Five independently generated HDF5 collections share an 8192-by-2048 logical
matrix: 16 stored values per row split into 1, 8 and 32 components; 128 values
per row split into 8 components; and the 16-value, 8-component matrix stored
with bounded gzip-1 chunks. All use float32 values, int32 indices and int64
stored offsets. Source row `r` has columns `step*k + r%97` and values
`1+(r+k)%31`; `step` is 127 for 16 values and 15 for 128. Every result is checked
against this formula outside operation clocks. The canonical FNV digest sorts
output column positions within each row outside clocks: it proves selected
logical CSR contents agree, not identical native sparse entry ordering or widths.

The first window covers component count and selector geometry, including a
selection wholly inside one component, crossing a component boundary and
reverse/duplicate positions and positive strides. Native all/range selectors
are used on both sides where they express the recorded positions; irregular
and duplicated positions use native integer-index arrays. The second covers all rows, all columns, higher
stored density and compression. The third covers repeated same-handle reads,
auxiliary checked preparations and separate C++ allocation/I/O diagnostics.
Eight primary comparisons have three fresh-process observations; five auxiliary
selector/storage controls have one observation and establish screening evidence
rather than stable latency estimates. The entire
schedule comprises 26, 16 and 24 processes, each window capped at 30 seconds
including startup/timeouts; no automatic retry of a started window is allowed.
The first contiguous-one-component observation runs in the screen window and
its remaining two observations complete in the reuse window, balancing the
established Python process-startup cost across windows. New fixtures are capped at 128 MiB. Partial schedules and failures remain evidence.

First-read clocks begin at dataset open and end after a fully materialized CSR
result. C++ includes selection, structural/sparse preparation, requirements and
caller-buffer allocation; Python includes native selection and allocation.
Imports, case parsing, formula verification and digesting are outside these
clocks but inside the campaign wall-time limit. Both use default positional
alignment: cross-component name checks are auxiliary C++ measurements. Neither
side skips structural validation to create an artificial comparison.

Repeated reads use the same open handle and selectors. C++ reuses its prepared
plan and destination buffers. Python repeats native `X[rows, cols]`, including
its native allocation. Setup is reported separately. This compares available
API/lifecycle costs, not equivalent allocation-free numerical kernels. The
repeated outputs are independently verified after each operation. No
zero-allocation or mmap speed claim follows from these measurements.

Build the small consumer against the immutable full Release prefix using CMake
module scanning, and generate fixtures with the existing reference environment:

```sh
E="$PWD/build/collection-matrix-NEW"
P="$PWD/build/optional-axis-validation-2026-11-17/candidate-prefix"
A="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow"
cmake -S tools/format-family-screen/collection-matrix -B "$E/consumer" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_PREFIX_PATH="$P" \
  -DArrowParquet_INCLUDE_DIR="$A/include" -DArrowParquet_ARROW_LIBRARY="$A/libarrow.so.2100" \
  -DArrowParquet_PARQUET_LIBRARY="$A/libparquet.so.2100"
cmake --build "$E/consumer" -j2
build/reference/full/bin/python3 tools/format-family-screen/collection-matrix/fixture.py "$E/fixtures-complete"
python3 tools/format-family-screen/collection-matrix/plans.py "$E" --prefix "$P"
```

The fixture generator refuses existing destinations and writes hashes and
physical sizes. `plans.py` freezes fixtures, selectors, consumers, source,
interpreters, libraries and diagnostic interposers. Initialize `state.json`
from the retained campaign checkpoint with all three window statuses
`not_started` before a separately authorized reproduction. Do not reset an
existing checkpoint or reuse its budget. Run each window serially with other
measurement/build workloads quiescent, using one recorded allowed CPU:

```sh
taskset -c CPU python3 tools/format-family-screen/collection-matrix/campaign.py screen "$E"
taskset -c CPU python3 tools/format-family-screen/collection-matrix/campaign.py volume "$E"
taskset -c CPU python3 tools/format-family-screen/collection-matrix/campaign.py reuse "$E"
python3 tools/format-family-screen/collection-matrix/summarize.py "$E"
```

The checkpoint is flushed to persistent storage before each window. The runner
sets six compute-thread limits to one and excludes interposition from primary
samples and Python. Normal OS/HDF5 caches are retained; fresh processes do not
mean cold storage. C++ counters report intercepted calls/bytes and `new`
requests, not physical device traffic, all allocators or live memory. These
samples describe a bounded screen, not tail latency or universal performance.
This suite does not qualify dense/CSC matrices or larger-than-memory throughput.
