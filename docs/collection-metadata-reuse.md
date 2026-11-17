# Collection metadata reuse, 17 November 2026

The metadata reuse change is implemented in `d479d5d`. Its baseline is
`7f8d72a`, whose production library is unchanged from `32852b6`.

Collection preparation now obtains the reference component's layout and types
from its existing selected prepared read. If that component is unselected, a
small compiled bridge validates its metadata once without scanning its numeric
payload. Empty selections, reference-based independent index/offset widths,
promotion and traversal order retain their existing behavior. Manifest opening
also checks the reference table's size once within the operation.

This reuses the existing descriptor, requirements and sparse scanner. It adds
no persistent cache, array owner, dependency or numerical batching. Backend
types remain private. All-component transfer preflight is unchanged: sharing
descriptors across that barrier would require additional ownership and budget
machinery, so that part was deferred. An unselected reference's metadata error
can now be detected later in preparation, still before destination writes.

## Bounded before/after comparison

One **11.953-second** serial window completed **22/22** processes: three fresh
timing observations per baseline/candidate/Python geometry, plus four separate
C++ diagnostics. Every complete value/index/offset oracle passed; canonical
digests matched and all frozen input identities remained unchanged. No second
window, profiling run, larger fixture or sparse-batching experiment was run.

| Median, milliseconds | Before | Metadata reuse | Python |
|---|---:|---:|---:|
| Small preparation | 2.686 | 1.289 | — |
| Small open through output | 10.911 | 7.982 | 5.701 |
| 10k preparation | 5.847 | 4.851 | — |
| 10k open through output | 23.480 | 22.368 | 36.701 |

Small-case preparation fell **52.0%** and end-to-end fell **26.8%**. Candidate
end-to-end samples were 7.924, 8.573 and 7.982 ms; baseline samples were 10.911,
11.857 and 9.719 ms. The candidate still takes **1.40 times** Python's small-case
median, a 2.281 ms gap. This is an improvement on the measured fixture, with
normal-cache and three-observation limits, not universal performance parity.

The 10k preparation median fell **17.0%**. Its end-to-end median fell 4.7%; sample
ranges nearly touch (candidate maximum 22.384514 ms, baseline minimum 22.424340
ms). This supports no strong general end-to-end claim. Independent phase
medians need not add to the end-to-end median.

Both C++ installations use Clang 18 Release with HDF5/Zarr/Parquet enabled.
Python is the existing SnapATAC2 2.10.0 environment, using simultaneous native
`X[rows, cols]`. Inputs, selected positions and complete CSR outputs match.
The small source is 256 by 256 in two components, selecting 64 by 64 with 512
nonzeros; small outputs all use float32/int32/int32. The established eight-part
10k-by-20k control selects 64 by 64 with 17 nonzeros; C++ retains int64 offsets
where Python returns int32. Canonical conversion and verification are untimed.

Small selector construction is outside both clocks; the 10k control retains
the original qualified C++ clock boundary in both builds. Imports, hashing,
goldens and close are excluded. C++ retains full numerical validation and
caller-buffer preflight; neither default C++ nor Python compares selected
axis names. These are available API comparisons with equivalent payloads and
different internal contracts, not identical kernels or language speed claims.
SnapATAC2 includes compiled Rust code.

The runner used CPU 0, six thread limits of one, normal OS/HDF5 caches and no
eviction. External activity is uncontrolled. Timing runs had no interposer;
Python received no HDF5 interposer. The
[machine record](../benchmarks/results/collection-metadata-reuse.json) preserves
samples, counters and hashes; the
[reproducible harness](../tools/format-family-screen/metadata-reuse/README.md)
reuses the existing independent oracles and fixture generators.

## Mechanism and next decision

| Counter | Small before → after | 10k before → after |
|---|---:|---:|
| Opening H5Oopen | 104 → 89 | 176 → 131 |
| Preparation H5Oopen | 75 → 36 | 183 → 144 |
| Preparation C++ new | 945 → 615 | 2,921 → 2,535 |
| Preparation H5Dread | 128 → 128 | 128 → 128 |
| Transfer H5Dread | 192 → 192 | 192 → 192 |

Preparation has zero string-payload reads in both versions. Its intercepted
read calls/bytes, and all recorded transfer counters, are unchanged. The small
transfer still takes 1.194 ms and retains 44 H5Oopen calls. Counters are logical
interposed activity and cumulative allocation requests, not physical device
traffic, every allocator or live memory. Diagnostic elapsed time is excluded
from performance claims.

The screen supports keeping this metadata change. **Production sparse batching
remains deferred.** The residual gap includes descriptor/preflight work as well
as short numerical reads. A further bounded experiment should distinguish
those costs and any byte/scratch tradeoff before adding a batching path; the
unchanged read count alone does not demonstrate a batching speedup.

## Validation and preservation

Debug passed **38/38**, including both installed import spellings. Focused
ASan/UBSan passed **7/7** with `detect_leaks=0`; this does not qualify leaks.
New tests cover independent reference widths when unselected, empty dense/CSR/
CSC selections, late/repeated reference visits, reverse traversal, tight-budget
fallback, malformed reference shape/encoding/version/buffer metadata with
untouched output, and corrupt numerical indices that are ignored only when
their component is unselected. Existing sparse and explicit name-policy tests
remain active. Independent source review found no confirmed defect.

The installed mmap audit verified actual read-only shared H5MU dense and H5AD
physical CSR mappings, retained native pointers after backed handles were
released, selected/logical mapping exclusions, and unchanged file bytes and
metadata. Mapping and numerical transfer implementations are unchanged. The
core archive is byte-identical to the baseline. The new internal bridge causes
an ordinary initial interface refresh; a later private implementation rebuild
preserved all **15 public BMIs and eight consumer objects** in both hash and
timestamp.

This is focused qualification, not a repeated full release interoperability,
single-backend, shared-library or concurrency matrix. Persistent raw receipts,
prefixes, patches and logs are under
`build/collection-metadata-reuse-2026-11-17/`, with a checksum-verified copy in
the primary checkout before managed worktree cleanup. Earlier evidence and
reference environments remain intact.
