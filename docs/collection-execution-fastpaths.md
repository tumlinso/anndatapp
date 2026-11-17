# Retained collection execution, 17 November 2026

Repeated CSR collection reads are substantially faster after retaining physical
component bindings and checked scanner resources. Interleaved duplicate reads
improve most. All four repeated-read workloads beat the tested native
SnapATAC2 API. The 32-component first read remains slower than Python, and one
preparation phase becomes more expensive.

The implementation is `fb518d9d5f1d23d5c1583b987902b635504f6fa0`, compared with
`5d21232` whose production library is `d479d5d`. The existing metadata reuse fix
is included in both sides. The [machine record](../benchmarks/results/collection-execution-fastpaths.json)
contains samples, phases, counters and hashes; the [probe sources](../tools/format-family-screen/collection-execution-fix/README.md)
describe reproduction. This completes the first resource-reuse step of the
[execution plan](collection-execution-plan.md), rather than a bulk-I/O rewrite.

## What changed

The move-only collection plan retains one checked physical binding per
participating CSR component. Ordered pieces refer to that component and keep
their immutable selections, so duplicates, source-entry order and output
identity are preserved without repeatedly acquiring the same dataset objects.
Numeric handles, minor-selector lookup and staging reuse existing
`TransferContext` ownership and the existing checked sparse scanner.

Each retained component's physical metadata is refreshed once per operation.
Every destination piece is preflighted before the first write; execution then
uses that checked binding. The scanner retains interior/boundary offset checks,
filtered-out invalid-index rejection, width conversion, explicit zeros and
overflow handling. Requirements-only and empty selections retain lazy codec
acquisition. Ordinary worker contexts retain their existing private backends.

Owned capacities and conservative HDF5 payload-cache/hash-table allowances are
charged to the existing planning budget. Components that cannot retain their
resources use the old execution path while other admitted components can remain
fast. If contexts would prevent the original piece/staging retention, contexts
are dropped first. No sieve setting, public tuning option, array model, thread
pool or global cache was added. Opaque HDF5 bookkeeping is not claimed to be
fully accounted by these estimates.

An intrusive, non-owning list lets collection close release retained HDF5
readers before closing component sessions. A new regression first reproduced
failure to reopen a component writable while a stale plan remained alive; the
drain-on-close fix now passes. Ordinary retained-owner lifetimes and stale-plan
errors are preserved.

The optimization is currently **CSR only**. Dense and CSC retain their prior
paths and fallback behavior. Public collection operations and axis-name policy
are unchanged. Backend types and implementation stay private; the public module
change is limited to a friend declaration and small compiled internal bridges
using existing owner/buffer types.

## Equivalent requested output

Both sides read the same existing immutable 8192 × 2048 CSR fixtures, selectors
and fully materialized output. Values are float32 and column indices int32.
C++ retains int64 offsets; Python returns int32. Each timing process's first
read and four repeated outputs pass an independent formula and matching
canonical digest. Sorting and canonical width conversion occur outside clocks.
Separate diagnostic processes perform three repeats and do not print native
widths; native-width equality is checked between C++ timing records.

First-read clocks cover open, selection, preparation, allocation and transfer.
Imports, parsing, verification and close are outside those clocks but inside
the campaign budget. Repeated C++ operations reuse the prepared plan and caller
buffers; Python repeats native indexing and allocates a new result on the same
open collection. These are available API/lifecycle comparisons, not identical
allocation-free kernels. C++ validation settings and widths are unchanged;
Python is SnapATAC2 2.10.0's native Rust-backed simultaneous `X[rows, cols]` API.

## Measured results

All times are milliseconds. Primary rows have three fresh-process observations.
Repeated medians are computed within each process, then across the three
process medians. Repeated operations exclude opening and first preparation.

| Workload | First read before → after | Python first | Repeated before → after | Python repeated |
|---|---:|---:|---:|---:|
| 32 components, scattered 64 × 64 | 69.203 → 59.936 | 49.120 | 15.768 → 5.103 | 6.120 |
| Interleaved reverse/duplicates, 64 × 64 | 64.889 → 15.793 | 23.332 | 27.363 → 0.822 | 2.178 |
| Eight components, 64 rows/all columns | 21.494 → 21.595 | 23.173 | 4.236 → 1.499 | 2.092 |
| Higher density, 256 rows/all columns | 24.854 → 22.276 | 27.460 | 5.880 → 3.224 | 3.965 |

Repeated reads improve approximately **3.1×, 33.3×, 2.8× and 1.8×** respectively.
The duplicate first read improves **4.1×**; the 32-component first read improves
13.4% but still takes **1.22×** Python's time. Opening is already faster in C++
on that case; remaining preparation/transfer overhead is not erased by the
repeated-read result.

Single-observation first-read controls favor the candidate: one component
7.717 → 6.791 ms (Python 16.198), contiguous selection 15.163 → 13.551
(Python 23.064), and gzip 30.013 → 23.246 (Python 27.164). These controls are
screening evidence, not stable latency estimates.

**Preparation tradeoff:** sparse-reuse preparation increases **4.486 → 5.458 ms**
(+0.972 ms, +21.7%). This is the sole phase crossing the agreed >10% and >0.1 ms
regression threshold; no full first-read or repeated-operation median crosses
it. Baseline preparation samples are 4.972/4.486/4.330 ms and candidate samples
5.458/4.829/5.549; ranges overlap. Preparation allocation requests/bytes also
increase, including 51,872 → 69,928 observed C++ allocation bytes. The new state
has an upfront cost, but these observations do not isolate its exact time
attribution. The complete first read is effectively unchanged for that workload.
The 32-component preparation also rises 16.754 → 18.290 ms (+9.2%).

## Counter evidence and the remaining I/O issue

These are separate instrumented repeated-read observations. Their durations are
excluded from timing claims.

| Repeated operation | H5Oopen before → after | C++ new requests before → after | Intercepted read bytes before → after |
|---|---:|---:|---:|
| 32 components | 704 → 256 | 10,208 → 2,368 | 844,544 → 0 |
| Interleaved reverse/duplicates | 1,232 → 32 | 12,320 → 296 | 4,460,160 → 0 |
| Sparse reuse | 176 → 64 | 1,496 → 592 | 1,053,248 → 0 |
| Higher-density reuse | 176 → 64 | 1,496 → 592 | 8,318,200 → 8,253,440 |

The first three cases have no intercepted libc read bytes in repeated reads,
consistent with retained handle/cache reuse. This is not zero physical I/O or
zero allocation. Numeric
H5Dread calls remain 192 in the 64-row cases and 768 in the 256-row case;
there is no row-batching claim. Retention also raises some preparation allocation
bytes and the first explicit preflight acquires lazy value handles.

Higher-density repeated reads still return about **8.25 MB** of intercepted
libc reads for approximately **266 KB** logically requested from sparse arrays.
Those totals include metadata/read-ahead, not measured disk traffic, peak memory
or evidence of full matrix materialization. The contiguous-data sieve remains a
hypothesis for amplification, without path/offset-attributed confirmation.
No third window, sieve comparison or policy change was run. This remains a
specific follow-up opportunity; the new timings do not establish optimal
selective I/O.

## Qualification and preservation

Two windows ran once, taking **18.596 and 16.770 seconds including input checks
and window-runner/workload startup**, totaling **35.366 seconds**. The outer
campaign interpreter's initial imports precede its recorded timer. All **53 processes** passed, all
354 frozen identities remained unchanged, and the 171-source-file candidate
manifest matched. CPU 2, six thread limits of one, normal OS/HDF5 caches, no
eviction and identical Clang 18 Release flags were recorded. External host
activity was not exclusively controlled. Three observations do not establish
tail latency, statistical certainty or a universal performance result.

Debug passed **38/38**, Release **39/39**, affected ASan/UBSan **7/7**,
HDF5-only **6/6** and Zarr-only **5/5**. Leak detection was disabled under the
runner's restrictions. Installed consumers cover both import spellings. New
regressions cover component/session/root identity, interleaved plans, raw sparse
duplicates/zeros, mixed widths, codec laziness, low-budget and hybrid fallback,
padded physical chunks, stale plans and close/reopen lifetimes.

The real installed-consumer mmap audit passed rooted H5MU dense mapping and
physical CSR data/indices/offset mappings, read-only shared `/proc/self/maps`
entries, pointer retention and rejection of ineligible mappings. H5MU payload,
size and mtime assertions passed. The audit records a final H5AD fingerprint,
but does not take a separate before/after H5AD metadata snapshot. Mapping
implementation and eligibility are unchanged.

A timestamp-only private implementation rebuild left **15 public BMIs and two
importing example objects** unchanged in hash and mtime. Initial internal bridge
declarations require the normal interface refresh. The core archive is
byte-identical to the baseline. No general import-time speedup is claimed.

Raw receipts, prefixes, source patch/manifest, failure logs, checkpoints and
audits remain under `build/collection-execution-fix-2026-11-17/`. The initial
compile errors and the reproduced close/reopen failure were fixed before
qualification. This is affected qualification, not a repeat of the complete
release/platform/format matrix or a release publication.
