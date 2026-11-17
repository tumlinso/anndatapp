# Optional dataset axis-name validation comparison

This compares the previous checked preparation with the new default and opt-in
checked preparation, plus Python's native simultaneous `X[rows, cols]` indexing.
The C++ probe includes the existing qualified collection probe, including its
independent exact-value oracle and FNV output digest. The Python route does not
perform the same selected-axis checks as the checked C++ routes. Each geometry
selects 64 rows and 64 columns from an H5AD collection: the small selection has
512 nonzeros; the spread 10k-by-20k selection has 17.

One serial 13.124-second window on 2026-11-17 used normal caches, one CPU of
affinity and six recorded thread limits of one, three alternating-order fresh-process timing observations per mode
and geometry, and six separate interposed diagnostic processes. All 30 processes
passed the goldens; shared FNV digests were `bb7805bcc25ffdde` (small) and
`54073d4a0e9be58f` (10k). The following are medians in milliseconds; bracketed
values are the three observations, in execution order for that mode.

| Geometry / mode | Preparation and requirements | Open through transfer |
|---|---:|---:|
| Small / baseline checked | 3.535 [3.857, 3.535, 3.413] | 10.847 [12.546, 10.847, 10.471] |
| Small / default | 2.056 [2.367, 2.042, 2.056] | 9.096 [10.831, 8.962, 9.096] |
| Small / checked | 3.555 [3.593, 3.555, 3.447] | 11.123 [11.126, 11.123, 10.260] |
| 10k / baseline checked | 22.933 [22.933, 22.702, 25.447] | 41.008 [41.008, 39.484, 45.722] |
| 10k / default | 5.564 [5.539, 5.603, 5.564] | 22.654 [22.654, 22.891, 22.594] |
| 10k / checked | 23.275 [23.275, 23.530, 22.596] | 40.272 [40.272, 42.015, 39.182] |

Python end-to-end medians were 5.698 ms small and 36.607 ms 10k; small
observations ranged 5.572–34.098 ms. Different validation and interface
boundaries preclude interpreting these as like-for-like speedups. In the
separately instrumented runs, preparation performed 128 non-string H5Dread
calls in each mode. String calls were 6 small and 18 large for baseline and
checked, and zero for default. Interposition counts calls, not physical I/O;
results are fixture- and environment-specific, and three samples do not bound
longer-term variance.

The recorded candidate `libanndatapp.a` SHA-256 is
`2678592fc437c3744915c0672943c458a7b7dd2840e113d1f1a4a67abe4be930`;
the baseline is `8122de5dd04065111433f305502bf408a960efbd8fcac1b8366b5a423dabaa38`.
The ignored local plan, receipt and summary SHA-256 values are respectively
`7b7afed7ba8518aaed822a335a13e76063ed5d331d44adf72dc20bc3fa37a5a1`,
`0209026874c7797eadbc8794d2c3108f9c2d4f8ca9bc85006b83b99539d66a5d`, and
`3769133cacb8e49b296e2743399e0416d30ffc592491e5b501a80c7d8ad323af`.
The receipt records all inputs and settings. Preserve it and any partial run;
do not overwrite or automatically repeat a failed window.

To reproduce with the existing fixture, library, and Python environment, from
repository root, choose a new empty evidence directory and build two consumers.
The Arrow paths below are specific to this environment. The candidate prefix
must be installed with the full Release HDF5, Zarr and Parquet configuration.
The baseline prefix must match the hash checked by `freeze.py`.

```sh
E="$PWD/build/optional-axis-validation-reproduction/comparison"
B="$PWD/build/collection-prep-fix-2026-11-16/candidate-prefix"
C="$PWD/build/optional-axis-validation-2026-11-17/candidate-prefix"
A="$PWD/build/reference/current/lib/python3.12/site-packages/pyarrow"
mkdir -p "$E"
for mode in baseline candidate; do
  if [ "$mode" = baseline ]; then prefix="$B"; option=OFF; else prefix="$C"; option=ON; fi
  cmake -S tools/format-family-screen/optional-axis-validation -B "$E/$mode-consumer" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_PREFIX_PATH="$prefix" -DHAS_AXIS_NAME_OPTION="$option" \
    -DArrowParquet_INCLUDE_DIR="$A/include" -DArrowParquet_ARROW_LIBRARY="$A/libarrow.so.2100" \
    -DArrowParquet_PARQUET_LIBRARY="$A/libparquet.so.2100"
  cmake --build "$E/$mode-consumer" -j2
done
python3 tools/format-family-screen/optional-axis-validation/freeze.py --evidence "$E" --candidate-prefix "$C"
python3 tools/format-family-screen/window.py --plan "$E/plan.json" --output "$E/receipt.jsonl" --case-timeout 5
python3 tools/format-family-screen/optional-axis-validation/summarize.py --evidence "$E"
```

Run the window with other benchmark and build workloads quiescent and outside
runtimes that restrict Python's event-loop wakeup. The plan uses the lexical
virtualenv interpreter path, disables interop wakeup, excludes interposition
from timing and Python, and limits the entire schedule to 30 seconds. The
summarizer checks completion, hashes, output, and expected read counts.
