# Optional collection axis-name checks, 17 November 2026

AnnDataSet positional X reads now trust the manifest's axis alignment by default.
Set `DatasetOptions::check_axis_names = true` to retain the previous selected-name
comparisons during preparation:

```cpp
DatasetOptions options;
options.check_axis_names = true;
auto dataset = open_h5ads("collection.h5ads", options);
```

This is independent of `BackedOptions::validation`, which still defaults to
`Validation::full`. Component presence, stored shapes, X layout/dtype, sparse
structure, destination capacity and backing lifetime checks remain on their
existing paths. Label selections still resolve names, `validate()` compares the
complete axes in either mode, and creation still requires common variable names.
The policy is selected per open/create and is not persisted into the manifest.

The implementation adds one public option, retains it in the existing collection
owner and gates the existing comparison call. It adds no read engine, array model,
cache or dependency. Numerical readers, mappings and caller-buffer transfers are
unchanged. The Release core archive is byte-identical to the baseline.

## Bounded comparison

The baseline is `d41c1f8`, using its qualified Release installation. Both C++
installations enable HDF5, Zarr and Parquet. Python uses the existing SnapATAC2
2.10.0 environment with simultaneous `X[rows, cols]` indexing. Each route opens the
same linked files, selects the same 64 rows and 64 columns and materializes the
same CSR values, indices and offsets. The small fixture has two components and
512 selected nonzeros; the 10k-by-20k fixture has eight components and 17.

One **13.124-second** serial window completed all **30 processes** within its
30-second cap: three fresh-process timing observations per mode and geometry,
plus six separate C++ diagnostic processes. Every independent formula oracle
passed and canonical output digests matched. Imports, digest conversion and
oracle checks are outside operation clocks. CPU affinity was one CPU, six recorded
thread limits were one, caches were normal, and no eviction was performed.

| Median, ms | Previous checked | New default | New checked | Python native X |
|---|---:|---:|---:|---:|
| Small preparation | 3.535 | 2.056 | 3.555 | — |
| Small open through transfer | 10.847 | 9.096 | 11.123 | 5.698 |
| 10k preparation | 22.933 | 5.564 | 23.275 | — |
| 10k open through transfer | 41.008 | 22.654 | 40.272 | 36.607 |

Default preparation fell **41.8%** and **75.7%**; end-to-end fell **16.1%** and
**44.8%**. The new default took **1.60 times** Python's small-case median and
**0.62 times** its 10k median. The small-case gap remains. Checked preparation
stayed within 1.5% of the baseline. Three observations are a screen, not a general
performance guarantee; Python's first small observation was 34.098 ms and all
samples are retained.

The payload comparison is equivalent, while the APIs still provide different
contracts: C++ retains destination preflight and full numerical validation, and
keeps int64 output offsets in the 10k case where Python returns int32. Neither
Python native X access nor default C++ preparation compares selected axis names.
These are package/API measurements; SnapATAC2 includes compiled Rust code.

Preparation diagnostics confirmed the mechanism. Axis-name string H5Dread calls
fell from **6 to 0** for the small fixture and **18 to 0** for 10k. All modes still
performed **128 non-string sizing reads**. Checked-mode counters matched the old
implementation exactly. Default C++ new requests fell from 1,295 to 945 and from
3,923 to 2,921. Intercepted calls and cumulative bytes describe logical library
activity, not physical disk traffic, every allocator or live memory. Diagnostic
timings are excluded from speed claims.

The [machine record](../benchmarks/results/optional-axis-validation.json) contains
samples, preparation counters, hashes and validation results. The
[probe and reproduction instructions](../tools/format-family-screen/optional-axis-validation/README.md)
reuse the existing qualified oracles and window runner. No second timing window
or larger workload was run.

## Validation and preservation

Debug passed **38/38**, including installed consumers using both import spellings.
Focused ASan/UBSan passed **7/7**, with leak detection disabled. The new tests cover
default and checked behavior for mismatched observation/variable names across
dense, CSR and CSC data, prepared and convenience transfers, materialization,
label resolution, unchanged rejected output, explicit validation and creation
policy propagation. Existing checked batching, reverse/duplicate selection,
sparse corruption, width, capacity and lifetime tests remain active. A read-only
independent source review found no actionable defects.

The installed mmap audit verified real shared read-only file mappings for rooted
H5MU dense values and physical H5AD CSR values, indices and offsets. Direct
pointers remained valid after backed handles were released. Selected dense and
logical sparse mapping rejection, file bytes and metadata were preserved.

This focused policy change did not repeat the full release interoperability,
shared-library or concurrency matrix. Raw receipts, immutable prefixes, fixture
hashes, source patches and logs are preserved under
`build/optional-axis-validation-2026-11-17/`; the earlier baselines remain intact.
