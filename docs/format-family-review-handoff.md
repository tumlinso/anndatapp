# AnnData format-family review handoff

Review the implementation at `93c1fa8f6f62a51acfe3f1a1a962701ff6f020e2` against baseline `e8c51ce7c3d8c2ded9d1b5f43c1e4d035ea13629`. The feature commit is `7999fa67a1b62fec7ce2533e135a4bd28f20d1d2`; the following commit records qualification. Later handoff/documentation commits do not change the reviewed code. The user authorized pushing local main to origin/main. Release publication and downstream migrations remain outside this request.

The purpose is to decide whether the implementation meets the original acceptance contract. Inspect the requirements and diff independently, then use the candidate regressions below to check the findings. Passing test totals do not establish complete coverage. Preserve unrelated work, and do not modify tracked implementation files during a review unless the user separately authorizes repairs.

## What was implemented

| Component | Intended scope |
| --- | --- |
| Shared storage and metadata | Rooted AnnData bindings in HDF5/Zarr, raw arrays/strings, groups and typed/nested attributes, flush/close and affected-root invalidation; append a heterogeneous sequence kind to Element. |
| MuData | Ordered modalities, global annotations, explicit axis maps, partial membership and duplicate-preserving selection; H5MU and Zarr. |
| TreeData | AnnData plus observation/variable trees, packed topology, typed/nested attributes, leaves/nodes/subset alignment, ancestor retention, overlap policies; H5TD and Zarr. |
| AnnDataSet | HDF5 H5ADS manifests linking H5AD components, lazy collection selections/transfers, common variables, relocation overrides, manifest copying with reference rebasing, explicit materialization. |
| SpatialData | Images, labels, points, circles/polygons/multipolygons, AnnData tables, instance links, per-level raster transforms, windows, vector row ranges, named-axis transforms/composition/inversion; private optional Parquet/GeoParquet adapter. |
| Packaging | Additional `import anndata;` facade with existing type identity; retain anndatapp namespace, package, targets and old imports. Parquet is off by default. |

The native models reuse existing arrays, strings, columns and tables. Public interfaces must not expose backend, HDF5, JSON or Arrow implementation types. Apply [CCell coding guidance](/home/tumlinson/CCell/coding_style.md), the AnnData++ ownership/I/O exception, [STYLE.md](../STYLE.md), and applicable AGENTS.md. Numerical loops should expose memory/layout and avoid allocations after acquisition. Prefer small compiled interfaces and explicit copies.

## High-performance access contract

The user's explicit requirement is load-only-what-you-need, eligible memory mapping, and direct pointer access for an HPC environment, preserving the pre-extension package's fast paths. Treat that as acceptance criteria for each new wrapper and codec.

- Inspect opening, names, dimensions, selection/view construction and per-element access separately from explicit read/copy/materialize. Identify any incidental full payload read, repeated decode, conversion or temporary numerical owner. Account for axis-map and annotation metadata costs rather than dismissing them as free.
- Preserve BackedArray descriptors and caller-owned dense/sparse destinations. Native and retained views must retain direct pointer/stride or values/index/offset access after acquisition. Numerical kernels must not acquire ownership, allocate or synchronize internally.
- Preserve `BackedArray::map_readonly()` and `MappedArray::data()`/`unsafe_data<T>()` through rooted bindings. An eligible mapping must be a real read-only, demand-paged file mapping with an explicit retained owner. Ineligibility must return an explicit error, never silently allocate and read the array.
- Keep baseline eligibility rules explicit. Full, native-endian, eligible dense physical arrays can map; arbitrary selections, compressed/encoded payloads and logical sparse groups cannot be assumed to map. Map eligible sparse physical data/indices/indptr buffers separately and retain all owners, or transfer selected values into caller storage. Do not introduce a parallel array model to hide that distinction.
- Compressed/chunked/Parquet reads should touch only required physical chunks or intersecting row groups, with bounded staging and explicit decode/output allocation. An explicit full materialization is different from an incidental whole-store read.
- Verify descriptor and mapping lifetimes, sparse widths, duplicate/order semantics and pointer eligibility at nested modality/table/raster roots. Coordinate close/replacement/truncation according to the existing mapping contract. The typed mapping extension retains its documented Linux/Clang and C++ object-lifetime limitations.
- Measure metadata-only and subset operations on representative large files and linked collections. Kernel timing alone does not qualify an HPC I/O path.

A focused source audit found that the rooted backend forwards the existing mmap and numerical transfer mechanisms; matrix/raster payloads remain explicitly backed. It also found three important inspection/acquisition costs to investigate: MuData full `view()` eagerly reads every modality's two axis maps; AnnDataSet open unconditionally scans every component's axis names, reading observation names individually; scalar-JSON TreeData names or keyed lookup parses and constructs all trees and attributes on every call. Some bytes are unavoidable because the scalar encoding is indivisible, but constructing every unrelated tree for a name query needs separate justification. Grouped Zarr tree reads can read a selected tree and selected attribute rows, while topology projection still reads the necessary topology.

SpatialData row-range reads decode intersecting Parquet row groups; shapes also probe the first geometry. Those costs and Arrow's own allocations must be visible in the assessment. None of these observations establishes a whole-family timing result. The [access-path audit](format-family-access-review.md) records the detailed source table and bounded runtime checks. Runtime verification confirmed a rooted dense H5MU mapping and individual physical CSR buffer mappings in read-only file-backed regions, retained after backed handles were released. Ineligible selections and logical sparse mapping rejected explicitly. This preserves the exercised old fast paths; it does not establish lazy behavior or mapping eligibility for every new operation. Raw source and runtime output are preserved under `build/format-family-qualification-2026-11-14/selective-io-fast-path-audit/` in the primary repository.

## Compatibility contract

- MuData 0.4.1 current H5MU/Zarr writes; legacy 0.3.10 reads, including missing-axis defaults.
- TreeData 0.4.0 HDF5 JSON and current columnar Zarr; legacy 0.2.5 scalar-JSON Zarr reads.
- SnapATAC2 2.10.0 H5ADS/H5AD manifests/components; no invented Zarr collection dialect.
- SpatialData 0.9.0: read storage 0.2, 0.3 and 0.4; write 0.4. WKB and admitted native GeoArrow geometry encodings. Retired ragged shape storage 0.1 is excluded.
- Preserve absent versus null attributes, duplicate selections, raw axes, nullable/categorical values, sparse widths, retained lifetimes and caller buffers.
- Unknown subtypes must not be silently flattened during materialization/export. Untouched same-store retention is a separate supported operation.
- Spatial table links use element-local instance identifiers independently of transforms. Valid dangling references are allowed. Exact raster level scale/translation must survive copies and writes.
- Arrow is private and optional. The native scalar/string/categorical column subset is supported; other documented Arrow extension payloads fail explicitly. Disabled payload capabilities and documented known preflight failures must be diagnosed before destination modification.
- General write failures are not transactional. Scratch limits cover documented library staging/validation, excluding retained metadata, caller/returned buffers and third-party internals. Selected noncontiguous sparse packing must fit its available budget.
- No raster resampling, scientific pipelines, general Python parity, external project edits or downstream pin updates.

See [format-family.md](format-family.md) for API details and the original contract represented by those APIs.

## Existing qualification and its limits

The author recorded passing Linux runs: Debug 33/33, Parquet/reference 49/49, core-only 20/20, HDF5-only 28/28, Zarr-only 26/26, shared 34/34, Release 34/34, ASan/UBSan 34/34 and focused TSan 4/4. Independent current/legacy reference packages generate inputs and reopen native outputs. An evidence check in the fresh review corroborated the accepted totals and distinguished earlier failed attempts from the later passing runs.

These checks missed the cases below. The current implementation therefore has unresolved acceptance gaps despite those genuine passing runs. ASan disables leak detection on this runner; TSan is a focused concurrency check. Neither arbitrary shared-session safety nor other platforms is qualified.

The [validation report](format-family-validation.md) and [machine-readable record](../benchmarks/results/format-family-qualification.json) contain the measured scope and hashes. The accepted implementation/test input digest is `180223eca95339848eb144bfa3bd312e8fe4f2196d4c6fdb44f1848e2f68dff8`.

## Initial independent findings, still unresolved

A fresh reviewer, using separate verifier contexts, reported and reproduced the following cases on the fixed artifact. The implementing agent inspected the preserved source and logs and consolidated them here. The reviewer's final narrative response was interrupted by a platform screening error, so this is a handoff of recorded findings, not an exhaustive completed audit. No implementation repair is included in this handoff.

1. **Polygon construction reads beyond a malformed geometry-offset buffer.** With one instance identifier, a one-entry geometry-offset array and omitted geometry types, `SpatialShapes::polygons` reads the next offset while inferring the type, before length validation. ASan reports a heap-buffer-overflow at `src/spatialdata.cpp:410`. Review checked-construction expectations and reject invalid lengths before inference. Evidence: `geometry/repro.cpp`, `geometry/repro-asan.log`, `geometry/README.md`.
2. **TreeData can replace an existing destination before a known sparse-packing budget failure.** A valid 1x1 CSR with 1,000 entries, selected with duplicate rows, fails at a 1,024-byte HDF5 write budget after replacing the marker file. Zarr reproduces at 2,048 bytes and removes the existing sentinel. The logs report `copy exceeds numerical buffer byte budget` and `original=0`. Tree-only preflight at `src/treedata_io.cpp:730` precedes store creation, but the AnnData packing check runs later in `src/backed_writer.cpp`. Check the documented known-scratch preflight promise separately from later I/O rollback exclusions. Evidence: `tree-budget.cpp`, `tree-budget.log`, `tree-budget-zarr.cpp`, `tree-budget-zarr.log`.
3. **TreeData Zarr materialization/export silently drops an unknown nested tree child.** Adding `/obst/alpha/future_nested` with `encoding-type=future-tree-extension` to a pinned input still allows the round trip, and the output omits the child. The columnar tree readers at `src/treedata_io.cpp:140` and the selected reader inspect known fields without rejecting other children. This conflicts with the explicit unknown-subtype/materialization requirement. Evidence: `tree-unknown/input.zarr` and `tree-unknown/output.zarr`; verify the added child and compare output keys.
4. **The private Parquet writer rejects valid native unsigned categorical codes.** `Column::categorical` accepts unsigned integer codes, but `arrow_array` handles only signed code types at `src/spatialdata_parquet.cpp:476`. A valid one-row uint8 categorical passes native validation then fails writing with `categorical codes require signed integer storage`. Check the intended categorical contract and conversion rules for all admitted native code widths. Evidence: `spatial_unsigned_category.cpp`, `spatial_unsigned_category.log`.
5. **SpatialData owner and view full validation differ, allowing a destructive late table failure.** A shape-correct sparse X with invalid offsets is rejected by owner `validate()` but accepted by `view().validate()`. The writer calls the latter, truncates an existing valid destination, then the nested AnnData writer rejects X. The reproduction prints `owner_rejected=1 view_rejected=0` and later `preserved=0 names=0`. Owner validation calls nested AnnData validation at `src/spatialdata.cpp:525`; view validation at line 580 omits it. Evidence: `validate/spatial_view_validation.cpp` and its log. Check all nested AnnData slots and both owner/view paths.

The first three and fifth findings warrant particular attention to memory or data preservation. The categorical case is an advertised capability gap. Establish final priorities from reproduction and the API contract rather than relying on these labels.

The reviewer also noted that nested SpatialData I/O and validation control flow is frequently compressed onto long single lines, which makes invariants difficult to inspect. Keep that maintainability concern separate from confirmed behavior defects and measured performance.

## Performance evidence and assessment questions

Measurements used Clang 18.1.3, Release optimization and an x86-64 Xeon E5-2698 v4 host:

- A fresh installed full-facade consumer build took 8.63 s for anndata and 8.69 s for anndatapp, each including generation of 15 consumer-local module interfaces. The consumer translation unit itself took approximately 0.05 s once those BMIs existed.
- Five affine calls on one million borrowed FP64 coordinate pairs took 7.36-7.60 ms. Allocation and initialization were outside the timed calls; sampled numerical checks passed. Separate allocation regressions cover fresh/repeated coordinate calls.
- Five retained raw HDF5 selections transferred 4 MiB into strided caller memory in 8.36-10.11 ms, median 8.66 ms. Each recorded 1,024 reads, exactly 4 MiB read, 4 KiB retained scratch under a 64 KiB limit and zero additional context scratch allocations. Every output value and stride gap was checked.
- A private implementation edit left all 15 observed public BMIs and two consumer objects unchanged; source restoration reproduced binary bytes.

Normal OS/HDF5 caches remained active. The import and coordinate observations preceded a private raw-geometry repair; the public interface hashes remained unchanged. Transfer measurements used the repaired library. These are narrow observations, not a comparative Python/C++ result or a whole-family large-data benchmark. MuData, TreeData, linked collection and Parquet end-to-end performance on representative large compressed data remains unmeasured. Evaluate acquisition costs, metadata growth, sparse packing and actual I/O call counts independently of an allocation-free numerical loop.

## Persistent evidence and reproduction environment

- Primary repository: `/home/tumlinson/anndatapp`.
- Managed checkout at the reviewed commit: `/home/tumlinson/.codex/worktrees/anndata-format-family/anndatapp`.
- Original full raw qualification: `/home/tumlinson/anndatapp/build/format-family-qualification-2026-11-14`.
- Review reproductions in managed checkout: `build/independent-family-review/`. A copy of the report sources/logs/fixtures is preserved in the primary evidence directory under `independent-review/`.
- Native build directories: `build/debug`, `build/parquet`, `build/release`, `build/sanitizers`, `build/core-only`, `build/hdf5-only`, `build/zarr-only`, `build/shared`, `build/thread-sanitizers` in the managed checkout.
- Full reference interpreter: `build/reference/full/bin/python3`. Legacy interpreters and six locks are described in `tests/reference/README.md`.
- Arrow prefix: `build/reference/current/lib/python3.12/site-packages/pyarrow`. Storage dependency prefix: `/home/tumlinson/anndatapp/build/dependencies/usr`.
- Use `/bin/bash`, `login:false` and an explicit working directory with shell tools. Keep new reproductions on persistent home storage, not /tmp. Put experiments in a separate review build directory; preserve existing logs and inputs.

The implementation and raw evidence are committed or stored persistently, with recovery bundles. Worktree archival remains blocked because the app considers it this chat's primary checkout. Do not manually delete it or shared Git metadata to bypass that protection.

## Requested review output

Report confirmed defects first, with severity, exact file/line, input trigger, observed/expected result and a minimal regression. Explicitly reject false positives. Distinguish compatibility checks, memory bounds, numerical performance, API maintainability and coverage gaps. State whether the original acceptance contract is met, and what evidence is missing. Do not turn optional style preferences into a broad rewrite or claim measured speed from source appearance.
