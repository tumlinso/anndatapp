# Ordinary Zarr selected-read performance, 16 November 2026

Ordinary chunked Zarr numerical selections now use the shared bounded chunk grouping and scatter engine. The supplied SpatialData raster request reads and decodes its one touched chunk once, replacing 1,024 repeated reads/decodes. The change applies through `BackedArray`, `PreparedRead` and transfer contexts; SpatialData still returns the same shared backed array.

The source base is `3de0d4580900868e13c5691dc9064e896933e057`; its shipping implementation matches `c0fefced2793e0edbf96567cbb99bac361b7070a`. The measurements qualified source on `codex/raster-selectivity`; recorded input hashes identify the tested implementation. The [compact record](../benchmarks/results/raster-selectivity.json) gives exact measurements, settings and receipt hashes. No merge or push was performed.

## Implementation and contracts

A private `NodeInfo` preference admits ordinary numerical Zarr chunks. HDF5 keeps its existing path. The existing grouping helper sorts only selected axis positions, reads each touched codec tile and scatters into distinct logical destinations without a Cartesian position map.

Ordinary one-shot reads use the existing unprepared backend decoder once per tile. This preserves missing-fill and codec-validation timing. Sharded one-shot reads retain their prepared reader, and transfer contexts retain their worker-private reader. No public API, decoded application cache, thread pool or SpatialData-specific reader was added.

Grouping reserves backend workspace/current retained capacity, per-axis maps/control and actual tile capacity. An early check declines grouping before control allocations when an existing packing tile exhausts the available allowance. The old logical-order fallback retains usable packing storage. Aliased outputs use that fallback; failures after scatter remain potentially partial. Caller buffers, signed strides, sparse widths, mappings, retained owners and caller-controlled manual/adaptive streaming remain in place. Ordinary one-shot metadata/codec setup still occurs per touched tile.

## Raster evidence

The unchanged fixture is uint16 shape `[1,256,256]`, ordinary Zarr v3 chunk shape `[1,256,256]`, little-endian bytes followed by Zstd. Selection is all channels, rows `16:80:2`, columns `32:96:2`; output is `[1,32,32]` / 2,048 bytes. Its encoded chunk is 131,084 bytes and decodes to 131,072 bytes. One full chunk decode remains necessary for this layout.

| Measurement | Shipping before | Candidate |
|---|---:|---:|
| End-to-end, uninstrumented | 385.842 ms | 4.944 ms |
| Transfer, uninstrumented | 381.018 ms | 0.646 ms |
| Selection and output allocation | 0.137 ms | 0.141 ms |
| Intercepted read calls, including open phase | 3,156 | 87 |
| Intercepted read bytes | 135,239,456 | 412,148 |
| Observed C++ new requests | 309,937 | 20,434 |
| Cumulative requested allocation bytes | 578,069,719 | 2,189,580 |
| Reads from the same encoded chunk file | 1,024 | 1 |
| Completed Zstd frames | 1,024 | 1 |
| Decoded bytes across those frames | 134,217,728 | 131,072 |

Timings are one fresh paired observation using the same rebuilt consumer source and matched Release dependencies. Diagnostics ran in separate processes. Before counters reuse the prior shipping diagnostic after checking its library and fixture hashes; they are not a newly repeated diagnostic. The candidate needed two streaming codec calls to complete its one frame, rather than the prior 2,048 calls for 1,024 frames.

The aligned-raster control initially crossed the 10% and 0.1 ms end-to-end flag, in its open phase. Two additional paired observations did not retain a material flag: three-sample end-to-end medians were 4.897 ms before and 5.244 ms candidate; transfer medians were 0.515 and 0.519 ms. The HDF5 sparse control observed 14.825 ms before and 16.169 ms candidate, below the stated combined flag; transfer was 7.502 versus 7.491 ms. These small observations do not prove universal absence of regressions.

## Bounded collection comparison

Collection library code was unchanged. The existing small common-matrix fixture returns the same independently checked 64 x 64 CSR result with 512 nonzeros through both APIs.

| Phase | Requirements + convenience read | One retained prepare_x |
|---|---:|---:|
| Open | 6.860 ms | 6.089 ms |
| Selection | 0.403 ms | 0.410 ms |
| Preparation/requirements | 14.189 ms | 13.953 ms |
| Output-buffer allocation | 0.003 ms | 0.003 ms |
| Read call | 21.569 ms | 8.094 ms |
| End-to-end | 43.024 ms | 28.549 ms |

The convenience read repeats preparation internally; the retained read still includes automatic preflight. Their read-call phases therefore perform different work. End-to-end compares the same requested output with validation preserved. Standalone preflight, measured separately, took 0.346 ms and left sentinel output buffers unchanged; it was not inserted into either compared read sequence.

Separate diagnostics observed 466 versus 331 total H5Dread calls. Each preparation phase accounted for 135 H5Dreads; the convenience read phase accounted for 327 versus 192 for retained preparation. The 135-call difference matches the removed repeat of preparation. Output-buffer allocation requested three allocations / 4,360 bytes. Selected-axis name checks, sparse requirement scans and their internal allocations remain combined within preparation; these measurements do not assign separate timings to them.

## Qualification and evidence limits

| Final configuration | Passed / run |
|---|---:|
| Debug | 38 / 38 |
| ASan/UBSan | 39 / 39 |
| Core-only | 22 / 22 |
| HDF5-only | 32 / 32 |
| Zarr-only | 29 / 29 |
| Shared | 39 / 39 |
| Targeted TSan | 4 / 4 |

All 203 final CTest executions passed. Tests cover ordinary v2/v3 raw/Zstd, endian/transpose, duplicate/reversed selections, negative/overlapping destinations, edges/fills, missing unsupported codecs, prepared reads, manual/adaptive blocks, tight-budget nonpacked fallback and writable-context read after write. Backend-disabled/shared installed consumers passed. The existing installed mmap audit verified actual read-only dense H5MU and physical CSR mappings and retained pointer lifetimes. All 15 public BMIs and two unchanged consumer objects retained both hashes and timestamps. ASan used `detect_leaks=0`; this is not leak qualification. The full 54-check Python interoperability campaign was not repeated.

Measurements ran serially on CPU 0 with OMP/OpenBLAS/MKL/NumExpr/Numba/Blosc limits of one, normal caches and no eviction. Fixture, dependency and source hashing may warm caches. Including superseded probe receipts and aligned confirmations, raster windows consumed 1.211 seconds and collection windows 0.554 seconds, each below its 30-second aggregate budget. No larger fixtures or Python timing campaign were run.

Read counters are intercepted calls/bytes, not physical device traffic. Allocation counters are cumulative observed C++ new requests, excluding other allocators; they are not live memory or RSS. Earlier pre-guard tests and probe receipts preceding a final harness-only cleanup are preserved as superseded and excluded from accepted results. The probes were rebuilt and the final short windows rerun to bind receipts to the final source.

Persistent recovery evidence is copied to `/home/tumlinson/anndatapp/build/raster-selectivity-qualification-2026-11-16`; working evidence is under `build/raster-selectivity-2026-11-16` in the managed worktree. It includes raw plans/results, source and artifact hashes, final/superseded logs, source snapshots, the patch, candidate prefix and mmap audit source. Frozen before prefixes and the previous investigation remain unchanged.
