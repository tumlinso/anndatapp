# Collection preparation fast paths, 16 November 2026

Collection preparation now reuses checked metadata within each operation and shares bounded manifest observation-name batches across component segments. Validation, numerical reads, sparse scans and mmap/direct-pointer access remain in place. The qualified implementation is `0c406e8`; its baseline is `a5213d2`, whose shipping library matches `180c440`.

The [machine record](../benchmarks/results/collection-preparation-fastpaths.json) retains samples, counters, identities and qualification results. The [reproducible comparison](../tools/format-family-screen/collection-fix/README.md) reuses the previous installed-consumer oracles and efficient Python indexing.

## Implementation

The common logical-shape and node helpers accept freshly inspected metadata or a known stored shape, preserving their full alignment and revision checks. A private table/index binding resolves encoding, version, rank and index shape once before selection/comparison. It is scoped to the current operation; returned public handles retain no metadata cache. Collection bridges bind the selected axes directly instead of constructing and selecting temporary public tables repeatedly.

Observation comparisons reuse the variable-comparison approach: hold one bounded reference batch and one peer, compare each intersecting component segment in logical selection order, and advance by actual returned prefixes. This preserves reverse, duplicate and interleaved component visits, prefixes ending inside a segment, long strings and the existing one-run fallback. Temporary coordinate capacities are checked against the existing selector allowance; the reader continues to use the paired sessions' scratch limits. No coordinate buffer grows with the entire selected axis.

The internal X preparation bridge selects and describes once, then calls the existing sparse requirements scan and returns the existing `PreparedRead`. The scan still checks offsets, filtered-out indices, widths, overflow and duplicates. Destination preflight still completes before deterministic failures can modify caller storage; later I/O failures retain the existing potentially partial-output contract.

The HDF5/Zarr numerical readers and mmap implementations are unchanged. The core archive is byte-identical to the baseline. There is no new array representation, persistent cache, tuning option, thread pool or dependency. Three small internal compiled bridge declarations were added using existing public types; ordinary implementations remain outside public module interfaces.

## Bounded before/after comparison

One serial **12.654-second** window completed all **22 processes**, below the 30-second cap: three fresh-process observations for each baseline/candidate/Python workload and four separate C++ diagnostic processes. Every value, index and offset passed the independent formula oracle; canonical digests match across languages and versions. All 34 frozen comparison identities remained unchanged.

Both C++ installations used Clang 18 Release, HDF5/Zarr/Parquet enabled and the same probe source. Python used the existing pinned SnapATAC2 2.10.0 environment with simultaneous `X[rows, cols]` indexing, without a selected-rows/all-columns intermediate. Inputs, selectors and fully materialized CSR outputs were identical. Imports, hashing and oracle verification are outside operation clocks; startup remains inside the campaign cap.

| Workload | Preparation before → after | End-to-end before → after | Python end-to-end |
|---|---:|---:|---:|
| Two-component small collection, 512 output nonzeros | 7.610 → 3.647 ms | 15.910 → 10.914 ms | 6.155 ms |
| Eight-component 10k collection, 17 output nonzeros | 38.794 → 25.383 ms | 58.499 → 44.938 ms | 38.645 ms |

Preparation improves **52.1%** and **34.6%**; end-to-end improves **31.4%** and **23.2%**. No measured phase or end-to-end median regressed by both more than 10% and 0.1 ms. Three samples are a bounded screen, not statistical certainty or a universal no-regression result.

Native Python X-only access remains faster: the candidate takes about **1.77×** its small-case latency and **1.16×** its 10k latency. Python does not perform C++'s selected observation/variable-name validation or caller-buffer preflight. That difference explains part of the work boundary; it does not establish parity or erase the remaining gap. In the 10k case C++ retains int64 offsets while Python returns int32; both use int32 indices and float32 values. Canonical digest conversion happens after timing. Transfer-only phase ratios perform different work and are not kernel speedups.

An untimed semantic check on copies of the small fixture confirmed the name-validation difference: native SnapATAC2 accepted a mismatched selected manifest observation name and a mismatched selected component variable name, returning the same verified matrix in both cases. C++ regression tests retain rejection of these mismatches before destination writes. This check adds no timing sample or speed claim; its reproducible source is [check_validation.py](../tools/format-family-screen/collection-fix/check_validation.py).

Preparation diagnostics support the intended mechanism:

| Counter | Small before → after | 10k before → after |
|---|---:|---:|
| HDF5 object opens | 244 → 105 | 790 → 273 |
| String H5Dread calls | 7 → 6 | 25 → 18 |
| All H5Dread calls | 135 → 134 | 153 → 146 |
| Observed C++ new requests | 2,980 → 1,295 | 9,786 → 3,923 |
| Cumulative requested C++ allocation bytes | 122,616 → 64,066 | 400,448 → 193,090 |
| Intercepted read bytes | 39,630 → 39,032 | 11,214,392 → 10,931,400 |

Both versions retain **128 non-string sizing reads** in each workload. The 10k string-call reduction is the expected seven removed manifest-reference reads; required labels and sparse structure remain checked. The improvement primarily removes acquisition work; the byte totals show that substantial HDF5 string work remains.

Runs used CPU 0, six recorded thread limits of one, normal OS/HDF5 caches and no eviction. Hashing and prior work may warm caches; external host activity is uncontrolled. Native HDF5 is 1.10.10. SnapATAC2 bundles native Rust code, so these are package/API comparisons, not C++ versus interpreted-Python claims. Python received no HDF5 interposer. Diagnostics count intercepted logical calls/bytes and C++ `new`, not physical-device traffic, all allocators or live memory; their runtimes are excluded from speed evidence. No larger fixtures or additional timing windows were run.

## Validation and preservation

Final Debug passed **38/38**, affected ASan/UBSan **7/7**, Zarr-only **5/5**, core-only **22/22**, and HDF5-only six checks plus two final focused checks (seven distinct cases). Installed consumers using both facades passed. ASan used `detect_leaks=0`, so this is not leak qualification. This was affected qualification, not a repeat of the full interoperability/shared/concurrency release matrix.

Regressions exercise malformed and empty indexes, selected/whole stored lengths, UTF-8 and long-name batching, zero selector-budget fallback, reverse/interleaved/duplicate selections, late mismatch with untouched output, rooted isolation/replacement/close, oversized aligned slots, sparse corruption and independent widths. An empty selection now explicitly tests that a shortened stored index behind an open snapshot still fails its full-length check.

The installed mmap audit confirmed real read-only shared file mappings for rooted H5MU dense data and physical H5AD CSR data/indices/offsets, including retained pointers after backed handles are gone. Selected dense and logical sparse mappings retain their explicit rejection behavior. Mapping did not modify file metadata. A subsequent private implementation rebuild left all **15 public BMIs and two consumer objects** unchanged in both hash and timestamp; the initial internal declaration additions require the normal interface refresh.

Raw logs, frozen sources/patches, prefixes, receipts and hashes remain under `build/collection-prep-fix-2026-11-16/`. Earlier baseline prefixes, reference environments and investigation artifacts were preserved. Two setup failures were corrected before acceptance: the initial internal declarations needed explicit public `Selection` qualification, and a new test had to close its extra reader before rewriting its fixture. Neither supplied accepted performance evidence.
