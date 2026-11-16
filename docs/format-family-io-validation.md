# Format-family I/O qualification, 15 November 2026

The bounded I/O changes are implemented through `c0fefced2793e0edbf96567cbb99bac361b7070a`, starting from `bbe693ce53a729cc8bc475cf56ee55bf1a33a666`. The pre-extension reference is `e8c51ce7c3d8c2ded9d1b5f43c1e4d035ea13629`. All 173 frozen implementation/test/build inputs match digest `026db73847482d6c932c1a9eeab23db0d490a36ca240723e72008980f4ea2953` after qualification. Reproducible screen sources, including the corrected reference-interpreter and point-identifier checks, are committed through `faf4e31`.

The [machine-readable record](../benchmarks/results/format-family-io.json) contains configurations, source/toolchain identities, artifact hashes and observations. This report supplements the historical [format qualification](format-family-validation.md) and [correctness-repair qualification](format-family-repair-validation.md).

## Implementation and review

`AnnDataSetView::prepare_x()` returns a move-only `PreparedAnnDataSetRead`. It checks the selected axes, saves exact requirements, and retains existing component `PreparedRead` objects through preflight and transfer. Requirements queries do not reconstruct their preparation. Convenience methods prepare ephemerally; materialization prepares once. Destination/staging failures are checked across the complete transfer before caller buffers change; later I/O failures remain potentially partial.

Retained descriptor payloads and actual capacities count against the planning allowance. Large or fragmented plans use bounded regeneration instead of an unbounded piece list. Shared source/session owners, caller buffers, allocator/control-block overhead and fixed acquisition headers are outside the staging allowance; this is not a process-memory or RSS bound. Collection close and manifest structural changes invalidate prepared views. Components are read-only through the collection, and independent external mutations require caller synchronization.

Selected index checks reuse each reference batch across participating components. HDF5 gathered strings use ordered point selections and the existing bounded string/reclamation machinery; contiguous selections keep the old run path, and tight budgets retain a one-string fallback. Variable comparisons retain only bounded component IDs, the reference and one peer table. This state is released before observation gathers. Sparse adjustment loops resolve their integer type outside the element loop.

SpatialData's validation/I/O phases now expose their control flow, and common table attributes have one construction helper. Native MuData slice projection/allocation costs are documented without changing its snapshot or thread-use semantics. Existing array, sparse, ownership, codec, transfer and adaptive/manual streaming representations remain the foundation; no general plugin, scheduling or array framework was introduced.

Independent source review identified and prompted fixes for an unbounded all-component bitmap, a tiny-budget contiguous-string regression and overlapping variable/observation staging. Focused rechecks found those reported issues resolved, with no consequential new finding in the changed paths. Runtime regressions cover the cases, including 180-byte string reads and a 248-component batch followed by 2,048 gathered rows under a 65,536-byte selector allowance. The review was source-only, not an exhaustive correctness or performance certification.

## Final validation

| Configuration | Passed / run |
|---|---:|
| Debug, Parquet off | 38 / 38 |
| Release, Parquet on | 39 / 39 |
| ASan/UBSan, Parquet on | 39 / 39 |
| Core-only | 22 / 22 |
| HDF5-only | 32 / 32 |
| Zarr-only | 29 / 29 |
| Shared, Parquet on | 39 / 39 |
| Full Parquet and Python interoperability | 54 / 54 |
| Targeted ThreadSanitizer | 4 / 4 |

All accepted rows have zero failures and skips. The unchanged pre-extension snapshot independently passed 20/20 native/installed-consumer checks. A separate MuData 0.3.10 missing-axis HDF5/Zarr conversion and current-reader reopen also passed. Existing current/legacy interoperability, malformed inputs, sparse widths/order/duplicates, selections, retained lifetimes and caller-buffer failure tests remain active.

Installed consumers exercise both facade spellings and the move-only prepared type, including backend-disabled configurations. ASan used `detect_leaks=0`; UBSan used `halt_on_error=1`. This is not leak qualification. ThreadSanitizer uses the repository's four-target transfer/streaming preset; building unrelated allocation-interposition tests under TSan fails with duplicate allocator symbols and is not an accepted gate. Earlier superseded runs and the mistaken all-target TSan attempt are preserved separately.

A final installed Release consumer verified actual read-only file mappings for nested H5MU dense data and physical CSR data/indices/indptr, retained pointer lifetimes after backed handles leave scope, unchanged source metadata and explicit selected/logical-sparse mapping rejection. Changing only `spatialdata_transform.cpp` rebuilt one implementation object while all **15 public BMIs and two consumer objects** stayed unchanged; restoration reproduced object/library/executable hashes exactly. This tests private implementation isolation, not public-BMI identity across API additions.

## Bounded screen results

The controls used frozen pre-extension, before-change and candidate Release builds with matching HDF5/Zarr dependencies and Parquet disabled for ordinary-core comparisons. Family consumers used matched before/candidate Parquet-enabled builds. Reference packages were AnnData 0.13.4, MuData 0.4.1, TreeData 0.4.0, SnapATAC2 2.10.0, SpatialData 0.9.0 and PyArrow 21.0.0. Native HDF5 is 1.10.10; h5py uses HDF5 2.0.0, so language attribution is confounded by backend differences.

Measurements ran serially on one recorded allowed CPU with common thread limits, ordinary caches and no eviction. Fixture hashing can warm the OS cache. Python timing ran outside the restricted runner with its normal event loop; the test-only periodic wakeup was disabled. Imports are reported separately from requested-result timings. Each core/format window was capped at 30 seconds, with one initial sample; only the flagged dense-read operation received two additional paired samples. Diagnostics used separate bounded windows and supply no comparative timing claims.

| Eight-component collection, selected 64 × 64 CSR | Before | Candidate |
|---|---:|---:|
| 100k observations: open through requested X output | 372.506 ms | 87.929 ms |
| 10k observations: open through requested X output | 352.997 ms | 76.031 ms |

All output values, indices and offsets match the independent fixture formula, with 26 and 17 output nonzeros respectively. The candidate's 100k prepared transfer alone took 12.011 ms, after 66.513 ms of preparation/allocation in that sample. The legacy operation includes additional internal preparation, so those phase timings do not form an equal-work speed ratio. The end-to-end observations above include both setup and transfer.

For the separate 100k end-to-end diagnostic, intercepted libc reads fell **3,823 → 1,132**, and intercepted read bytes **189,098,480 → 36,114,080**. Intercepted H5Dread calls fell **3,012 → 349**. Observed C++ `new` requests fell **87,519 → 22,227**, requesting **2,063,252 → 828,876 bytes**. These are intercepted calls and cumulative allocation requests, not physical device bytes, total allocations, retained memory or process peaks.

The ordinary-core screen completed 27/27 process records with matching independent output checks. One initial eager H5AD timing flag did not reproduce: three-sample medians were **6.124 ms pre-extension, 6.187 ms before and 6.170 ms candidate**. The initial plus confirmation core windows used 2.624 seconds. Other exercised core operations had no material flag under the stated 10% and 0.1 ms screening rule. This is bounded evidence of preserved paths, not proof of zero regression on every workload. Four-worker manual/adaptive controls were pinned to one CPU and do not establish parallel scaling.

### Python-family requested-result observations

| Requested result | Before C++ | Candidate C++ | Python |
|---|---:|---:|---:|
| AnnData selected matrix | 16.141 ms | 15.542 ms | 16.569 ms |
| MuData modality matrix | 30.974 ms | 28.236 ms | 36.485 ms |
| MuData selected maps | 10.087 ms | 10.305 ms | 38.693 ms |
| TreeData selected matrix | 16.310 ms | 16.394 ms | 22.183 ms |
| TreeData selected tree/data | 35.891 ms | 36.644 ms | 30.230 ms |
| Small linked collection matrix | 134.840 ms | 42.154 ms | 6.363 ms |
| SpatialData table matrix | 46.459 ms | 46.325 ms | 187.144 ms |
| SpatialData raster window | 356.754 ms | 363.362 ms | 291.070 ms |
| SpatialData point batch | 8.615 ms | 7.017 ms | 272.134 ms |

All 27 retained family timing records passed their output checks. These are single observations of equivalent requested results, with different available eager/lazy APIs, validation, allocation and preparation work. They do not establish equal-work transfer or language-wide speed ratios. The common collection case deliberately retains the old requirements-plus-one-shot convenience sequence; explicit preparation avoids its repeated planning. Python does not establish matching selected-axis validation/preflight. Python point selection computes its Dask frame before slicing; C++ reads the bounded batch. Python SpatialData table selection also loads the related labels group needed by its table linkage.

Two single-sample open-phase flags (MuData map and TreeData) did not produce material end-to-end flags. No family candidate end-to-end observation exceeded both thresholds against its before build. The first AnnData Python attempt used a wrongly resolved base interpreter; it failed before import and is excluded. The first C++ point oracle conflated pandas row indices with the separate `spot_id` instance-key column; those failed samples are excluded, and the corrected case checks both independently. Original plans, sources and failed receipts are preserved.

Five Python HDF5 diagnostic cases failed only under the interposer with H5Dread errors. Their counters are unavailable, not zero. Uninstrumented timing cases passed. The counter resolves H5Dread using RTLD_NEXT and returns an error if unresolved; the evidence does not establish the particular symbol-scope or version cause. C++ diagnostics, Python SnapATAC2 and Python SpatialData diagnostics passed. No instrumentation rewrite or further campaign was attempted.

## Remaining measured concerns and recovery

**Raster selectivity needs a separate focused follow-up.** The requested 1 × 32 × 32 uint16 result is 2 KiB, yet both before and candidate diagnostics intercepted **3,156 reads / 135,239,456 bytes**, with **309,937 observed C++ new requests / 578,069,719 requested bytes**, including open. These are cumulative operation counts, not physical disk traffic or live memory. The behavior is unchanged in these two family builds and is not established as a regression caused by this patch. It is material I/O amplification and must not be hidden by a claim that all formats are now efficient.

The small collection's remaining validation/preparation cost versus SnapATAC2 is another follow-up. The subsequent scoped implementation and same-fixture measurements are recorded in [the collection preparation follow-up](collection-preparation-io-validation.md). Agree a targeted mechanism and time/I/O budget before deeper measurements or extending the changes; no larger campaign was run here.

Persistent recovery evidence is under `/home/tumlinson/anndatapp/build/format-family-io-qualification-2026-11-15`: exact source archives/input manifests, frozen prefixes, accepted/superseded logs, fixture inventories and small family stores, raw measurements, failed harness attempts, mapping/rebuild receipts and a source recovery bundle. Raw reports, biological stores and binaries remain ignored. No required recovery input relies on `/tmp`. Release publication and downstream changes remain outside this work.
