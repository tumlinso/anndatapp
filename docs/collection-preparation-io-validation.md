# Collection preparation I/O follow-up, 16 November 2026

Collection preparation repeatedly acquired index metadata and HDF5 numerical resources. Two private shared-engine corrections now reuse checked selected-index nodes and retain existing HDF5 numerical handles within each sparse scan. The baseline is `9505d1f94085c985210c8268eb4068aec889fd78`; the measured candidate is bound by `candidate-inputs.json` and `candidate.patch` in the local evidence directory. The compact record is `collection_preparation_followup` in [format-family-io.json](../benchmarks/results/format-family-io.json).

## Implementation and validation

Selected-index comparison uses the existing checked node and cursor length. Component-budget preparation checks IDs, session/revision validity and the minimum scratch allowance without rebuilding selected tables. Reference batches remain outermost, with one peer at a time; empty reference axes still inspect every peer. Encoding/version/rank, selected length, UTF-8, bounded gather/run fallback, mismatch categories and collection error remapping remain checked. Supplied index metadata is no longer copied unnecessarily. With multiple invalid inputs, the first reported defect can change.

Sparse scans lazily acquire offsets, indices and values through existing HDF5 transfers and reuse their rank-one coordinate vectors. Values are acquired only when actually read. A private HDF5-only capability, forwarded through rooted stores, leaves Zarr and non-preparing wrappers on their ordinary fallback. Offset/index/value read order, interior/boundary checks, filtered-out invalid indices, widths, overflow, duplicate multiplicity, source entry order and boolean canonicalization are preserved.

Handles are local to one scan and released before its backend/session. There is no persistent cache, new public API, worker thread or retained payload. Existing HDF5 internal caching benefits from the dataset-handle lifetime; third-party internal memory remains outside the library payload-scratch hint. Caller synchronization against external mutation, prepared-plan invalidation and direct-memory capabilities are unchanged. Validation and destination preflight precede collection output writes; subsequent I/O failures retain the existing potentially partial-output behavior.

The required Debug suite passed **38/38**. A subsequent test-only unique-path adjustment passed its focused Debug rerun. Focused ASan/UBSan passed **7/7**, HDF5-only **6/6**, and Zarr-only **4/4**. ASan used `detect_leaks=0`; UBSan used `halt_on_error=1`. Regressions cover empty/malformed peer indexes, multiple name batches, reverse/duplicate selectors, untouched output after mismatch, CSR/CSC duplicate source entries, corrupt interior offsets, filtered-out invalid indices and missing Zarr chunks with unsupported codecs. Existing tiny-budget, invalidation and fallback tests remain active. Source review found no production defect; its missing empty-peer regression was added and passed.

All **15 public BMIs and two consumer objects** retained their hashes and modification times. The installed Release consumer built and passed the same sparse output oracle. This was focused qualification, not a repeat of the nine-configuration interoperability campaign.

## Same-fixture measurements

The unchanged two-component fixture has 256 x 256 source dimensions; selecting rows and columns `0,4,...252` yields 64 x 64 CSR with 512 nonzeros. All 512 values and indices and 65 offsets were checked against the independent formula. Runs were serial on CPU 0, with six recorded thread limits set to one, normal OS/HDF5 caches and no eviction. Hashing and preceding work can warm caches. Native HDF5 is 1.10.10; builds use Clang 18 Release. Shipping prefixes share their backend/Parquet configuration; paired phase builds are both HDF5-only.

The single bounded window completed **40 process records in 5.203 seconds**, with no failed records. Each timing row below is the median of three fresh-process observations. Interposed diagnostics are separate and their elapsed times are excluded.

| Shipping API phase | Baseline | Candidate |
|---|---:|---:|
| Explicit preparation and requirements | 14.651 ms | 7.435 ms |
| Prepared transfer, including destination preflight | 8.262 ms | 1.348 ms |
| Explicit prepared end-to-end | 30.105 ms | 15.232 ms |
| Standalone destination preflight | 0.361 ms | 0.379 ms |
| Convenience requirements plus one-shot read, end-to-end | 43.711 ms | 23.201 ms |

Convenience calls still prepare independently. Standalone preflight checks requirements and unchanged sentinel buffers; its payload golden is not applicable. No measured shipping phase exceeded both the 10% and 0.1 ms regression thresholds.

| Diagnostic-build preparation attribution | Baseline | Candidate |
|---|---:|---:|
| Selected-axis validation | 6.793 ms | 4.305 ms |
| Sparse sizing | 5.287 ms | 0.479 ms |
| Other preparation | 2.688 ms | 2.709 ms |
| Caller output allocation, separate | 0.00235 ms | 0.00230 ms |

Phase clocks give approximate attribution. Active versus disabled clock-hook preparation medians differed by +2.57% on baseline and -2.13% on candidate; exact diagnostic counts match their disabled-hook controls. Every per-sample exclusive phase partition reconciles with the inclusive preparation total. The other-preparation row includes the external requirements-copy time; its one 16-byte allocation is reported separately in JSON. Inclusive nested scopes must not be added twice. Candidate numerical-access scopes exclude scan-local handle destruction, which remains inside sparse sizing.

| Preparation diagnostic | Baseline | Candidate |
|---|---:|---:|
| Validation object opens / attribute reads | 211 / 428 | 126 / 254 |
| Sparse-sizing object opens | 256 | 8 |
| Sparse-sizing datatype / dataspace / property-list acquisitions, each | 128 | 4 |
| Total intercepted reads / bytes | 149 / 325,574 | 25 / 39,630 |
| C++ new requests / requested bytes | 5,306 / 177,014 | 2,980 / 122,616 |

Required H5Dreads remain **7 for validation plus 128 for sizing**, and **192 for transfer**. Sparse sizing intercepted reads fell 128 to 4; transfer reads fell 192 to 6. This supports reduced acquisition and repeated fetching with existing HDF5 caching, rather than skipped logical reads or validation. Interception counts describe observed calls and requested bytes, not physical-device traffic, all allocators, live memory or RSS. Both component plans remain retained.

Native preparation-only CPU sampling completed 64 warm repetitions per version, with 935 baseline and 538 candidate samples and zero lost samples. Named libc allocator self-cost was 3.32% and 3.72%; HDF5 memory-management self-cost was 7.91% and 6.51%. Unresolved leaves were 16.47% and 17.29%. These disjoint sampled instruction-pointer classes are incomplete attribution, include profiler/control overhead, and overlap the phase times. They do not isolate allocator wall time or establish a fresh-call speedup; C++ allocation-request reductions are separately measured above.

## Evidence and limits

Reproducible diagnostic sources and build instructions are under [collection-preparation](../tools/format-family-screen/collection-preparation/README.md). Raw plans, receipts, profiles, source identities, archives and build/test logs remain ignored under `build/collection-preparation-2026-11-16/` in the managed worktree. Source snapshots from two failed copy-only instrumenter smoke attempts are preserved; neither ran a fixture or consumed a measurement window.

This establishes the mechanism and benefit on one small fixture. No 100k-row rerun, larger fixture, Python comparison, cold-cache claim, broad performance campaign or universal no-regression claim is made.
