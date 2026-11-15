# Format-family repair validation, 15 November 2026

The repairs are implemented in `bad691a8fe4a83452fbe882720feee8583d79733`, based on `3a35c6f051cbd9125a75277d4b32c0a8616b5940`. The frozen set of 172 implementation, test and build inputs has digest `f1dd364cf820886bdb49e91cf32b310abd2318d6afca684d8c14991658bfac2d`; every input was rechecked after testing and the private rebuild probe. The [machine-readable record](../benchmarks/results/format-family-repair.json) records configuration identity, evidence hashes and individual screen observations. This supplements the historical [initial qualification](format-family-validation.md) and [review handoff](format-family-review-handoff.md).

## Correctness repairs

| Finding | Change and regression coverage |
|---|---|
| Polygon offset overread | Check offset lengths/chains before geometry-type inference. Malformed construction is tested under ASan/UBSan. |
| TreeData destructive late failure | Shared AnnData validation and bounded sparse-copy footprint preflight run before either HDF5 or Zarr destination creation. Python tests verify existing destination hashes survive known invalid-source and insufficient-packing failures. |
| Unknown TreeData child dropped | Reject unknown grouped tree children before full, keyed, selected or topology-only materialization. Poisoned-child fixtures exercise these paths. |
| Unsigned categorical output rejected | Read categorical codes through checked `code(i)` and build private Arrow int64 indices, preserving native widths, dictionary and ordering. All unsigned widths, selected/pruned dictionaries and upstream reopen checks are exercised. |
| SpatialData owner/view mismatch | Share compiled AnnData/table/column validation and serialization preflight. Nested invalid X and insufficient-capacity cases fail before destination creation. |

Independent repair review found a further shape owner/view discrepancy: invalid UTF-8 identifiers and excess backing buffers were not rejected consistently. Owner validation now delegates to view validation; retained source-buffer counts are checked separately from logical selected rows. Valid selected circles remain supported. A focused source recheck found the reported gap resolved; runtime evidence comes from the matrix below. This was a repair-focused review, not an exhaustive proof of the family implementation.

The shared preflight queries stage bounded selection indices without copying numerical payloads. Existing contextual categorical encoding diagnostics remain intact. General later I/O or codec failures still have no whole-store rollback guarantee.

## Acquisition and memory paths

MuData view/slice construction retains descriptors without reading maps. Each requested modality/axis map is acquired and cached independently. Backed content revisions detect in-place and transfer writes; relevant map/structural edits invalidate old lazy views. Already acquired storage remains retained for its documented lifetime. Regressions cover unused poisoned maps, repeat access, edits, replacement and close behavior.

AnnDataSet open reads manifest/component metadata and checks presence, shapes and X compatibility. It defers full axis-name comparison. Explicit `validate()` compares complete axes in bounded batches. Selected transfers compare the relevant component axes and preflight every affected transfer before writing the caller buffer. Tests cover deferred variable mismatches, selected-use errors, invalid later-component buffers and unchanged caller output on deterministic preflight failures.

Scalar-JSON TreeData still reads its indivisible scalar payload; names/key lookup scans encoded spans and constructs only requested trees. It has no hidden full-tree cache. Grouped selected reads retain their existing selective decoding. Spatial geometry kind uses admitted metadata where available; empty or later-range reads avoid unrelated first-record decoding. Older metadata without a kind still needs the documented inference fallback.

An external installed Release consumer verified rooted H5MU dense pointers in read-only file mappings, retained values after backed handles left scope, and unchanged source size/mtime. It also mapped physical CSR `data`, `indices` and `indptr` through the generic store without numerical `read()`/`copy()`. Selected dense and logical sparse mappings retain explicit rejection diagnostics. This checks these HDF5 paths; it does not extend mmap eligibility to compressed or transformed data. Existing Zarr/backend tests remain part of the matrix.

Changing only `src/spatialdata_transform.cpp` rebuilt one implementation object, with **15 public BMIs and two consumer objects unchanged**. Restoring it restored object/library/executable hashes exactly. This proves implementation isolation for that edit, not BMI identity across the intentional public API additions in this repair.

## Final build and interoperability gates

| Configuration | Passed / run |
|---|---:|
| Debug, Parquet off | 37 / 37 |
| Release, Parquet on | 38 / 38 |
| ASan/UBSan, Parquet on | 38 / 38 |
| Core-only | 22 / 22 |
| HDF5-only | 31 / 31 |
| Zarr-only | 29 / 29 |
| Shared, Parquet on | 38 / 38 |
| Full Parquet and Python interoperability | 53 / 53 |
| Targeted ThreadSanitizer concurrency | 4 / 4 |

Installed consumers cover both import spellings. ASan used `detect_leaks=0`, so this is not leak qualification; UBSan used `halt_on_error=1`. The concurrency row covers existing worker-local transfer, streaming and threaded Python checks, not arbitrary shared-session safety. All accepted rows have zero failures and zero skips.

The full reference environment resolved AnnData 0.13.4, MuData 0.4.1, TreeData 0.4.0, SnapATAC2 2.10.0, SpatialData 0.9.0 and PyArrow 21.0.0. Current/legacy format generators and locks remain in `tests/reference`; a separately pinned older TreeData environment and existing SpatialData legacy fixtures were used, and separate focused MuData legacy checks also passed. Fixture summaries and accepted CTest output hashes are in the record. No existing interoperability environment was upgraded.

Builds use Clang 18 and the existing CMake presets/configurations. Rebuild with `cmake --build build/<configuration>` and run `ctest --test-dir build/<configuration> --output-on-failure`; the concurrency gate uses `-R '^(transfer_context|streaming|python_streaming_threaded|python_completeness_threaded)$'`. Follow the original qualification's explicit dependency/reference options when configuring anew. In this restricted runner, Python Zarr checks need `ANNDATAPP_INTEROP_ZARR_WAKEUP=1` and a persistent writable `TMPDIR`. Initial runs missing the wakeup were terminated; only the successful retries are accepted. Earlier transient build failures during concurrent source edits are also retained as rejected attempts.

## Initial performance screen

One serial screen completed in **25.297 seconds**, under a 30-second total budget and a three-second per-process cap. Fixtures are uncompressed CSR, 10k/100k observations by 20k variables, 100 stored values per row, eight linked components and identical monolithic controls. Total fixture storage is **213,446,160 bytes**. Reproducible sources are in [tools/format-family-screen](../tools/format-family-screen/README.md).

| Candidate collection operation | 100k observations | 10k observations |
|---|---:|---:|
| Open | 14.878 ms | 14.623 ms |
| Inspect metadata after open | 1.236 ms | 0.981 ms |
| Positional view after open | 0.388 ms | 0.362 ms |
| Selected 64 × 64 CSR caller-buffer transfer | 180.508 ms | 181.546 ms |

Transfer timings exclude requirements, selection construction and output allocation; complete candidate transfer processes took 383.7/377.8 ms. Candidate collection shape, types, nnz and checksums matched both monolithic controls. These are comparison checks, not an independent semantic oracle. Native and reference regression tests supply separate correctness evidence.

Twenty of 28 processes completed. Eight baseline collection processes timed out before emitting operation results, leaving six complete baseline/candidate pairs: monolithic open, monolithic read and physical CSR mapping at each size. A timeout covers all setup and cannot supply a baseline operation latency or measured speedup. Monolithic selected reads were 9.010 → 8.852 ms at 100k and 9.406 → 8.837 ms at 10k. Mapping succeeded in both versions; sampled first/warm-touch checksums agreed for all three CSR buffers. These single samples provide no variance or regression estimate.

Normal OS/HDF5 caches were active, without eviction; first touch is not necessarily cold. RSS and fault counts are process-level counters. Physical I/O bytes, allocation counts and retained scratch were **not measured**. All recorded major-fault deltas were zero, which does not imply no I/O. The root paused its own builds/tests for measurement; external host activity was not controlled.

The useful flag is collection subset-read overhead against the monolithic control: about 181 ms versus 9 ms in this screen. Proposed next work is one bounded call-count/profile of selected-axis validation and transfer preflight, using the same fixtures and buffers, then deciding whether a repeated-read test is warranted. Agree time/I/O budgets with the user first. No deeper campaign or optimization was started.

## Recovery and handoff

Persistent local evidence is preserved under `/home/tumlinson/anndatapp/build/format-family-repair-qualification-2026-11-15`, including source-input and fixture manifests, accepted and rejected logs, mmap reproduction source, private rebuild receipts, raw screen results and a source recovery bundle. Generated stores, environments and raw reports remain ignored. The frozen baseline prefix is preserved in the managed worktree; the evidence receipt identifies its artifact hashes. No required recovery evidence relies on `/tmp`.

Reviewers can start from the source commit and five regressions above, then inspect acquisition invalidation, bounded preflight accounting and the collection-read cost flagged by the screen. Unsupported format/capability boundaries in the format guide remain unchanged. Release publication and downstream migrations are outside this repair.
