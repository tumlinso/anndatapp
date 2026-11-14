# AnnData++ 1.0 support contract

This describes the 1.0 capability boundary. Qualification scopes and evidence are tracked in [v1-status.md](v1-status.md).

The current source also contains the four format-family APIs and selected file-version bounds described in [format-family.md](format-family.md). Their independent qualification evidence is recorded separately; the source-level support list is not a qualification result.

## Data and storage

| Capability | Core | H5AD | Zarr v2/v3 |
| --- | --- | --- | --- |
| Boolean, fixed-width integer, FP32 and FP64 dense arrays | Native typed pointers, arbitrary rank and signed strides | Declared array encodings | Declared array/codec encodings |
| CSR/CSC | Independent value/index/offset types; duplicates, unsorted entries and explicit zeros retained | Declared sparse encodings | Declared sparse encodings |
| UTF-8, nullable and categorical columns, tables and nested metadata | Native owners and captured views | Declared element encodings | Declared element encodings |
| AnnData alignment, selections and raw | Two annotated axes; raw variables remain independent | Declared AnnData/raw encodings | Declared AnnData/raw encodings |
| Native record arrays | Field buffers or interleaved members; explicit text lifetimes | Primitive numerical/text compound subset | Contextual rejection; no lossy substitute |

The exact encoding versions and limitations are in [H5AD](h5ad.md), [records](records.md) and [backed storage](storage.md). Core representability does not imply serialization support. Unknown required encodings, codecs and unsupported conversions produce contextual errors. Same-store retention of untouched extension data is distinct from eager materialization or cross-store opaque-data preservation.

The package boundary contains no Python runtime, GPU execution or FP16 storage. Composed biological formats, remote object stores, ragged/complex values and additional encoding families are outside this cleanup. New adapters follow the [extension contract](extension-contract.md) and reuse the existing logical representations.

## Format-family source APIs

The source exports `anndatapp.mudata`, `anndatapp.treedata`, `anndatapp.dataset`, and `anndatapp.spatialdata` through both `import anndatapp;` and its `import anndata;` facade. These modules add native construction, validation, selection, explicit copy, and the documented backed operations. The detailed read/write version table and limitations are in [format-family.md](format-family.md).

MuData and TreeData use the existing HDF5/Zarr backends. AnnDataSet is the SnapATAC2 `.h5ads` HDF5 manifest format with linked H5AD components; its selected implementation does not define Zarr collection files. SpatialData uses Zarr storage. Its vector elements need the optional private Arrow/Parquet adapter, disabled by default with `ANNDATAPP_ENABLE_PARQUET=OFF`. Core arrays, tables, rasters, and transforms remain available without that adapter; vector-file operations report an explicit capability error.

## Access, ownership and costs

Large owners are move-only; `copy()` creates independent data. Retained views preserve their bindings and allocation lifetimes, while edits through existing numerical aliases can remain visible. Borrowed numerical buffers remain the caller's lifetime responsibility. Metadata acquisition and explicit copying may allocate; returned numerical pointers expose the actual extents and strides. `BackedStore` exposes rooted AnnData and generic store elements while keeping the physical backend private; explicit close invalidates acquired backed descriptors.

`Validation::full` checks the documented boundary invariants. It cannot prove the lifetime, allocation extent or absence of external races of arbitrary supplied pointers. Trusted paths deliberately omit checks; they do not make malformed buffers valid. Byte-order metadata is separate from native C++ type identity.

Use focused column/record acquisition and reusable worker contexts for repeated work. Whole table/dataset views capture structural bindings. Non-contiguous sparse-source writes may explicitly materialize the selected sparse result. Scratch limits account for specified library staging, not caller output, process RSS, all metadata or third-party codec/HDF5 allocations.

## Execution and failures

Callers own scheduling and synchronization. Prepare backed handles and contexts serially; give each concurrent worker its own context and output storage. Shared direct session operations and transformed categorical plans require caller serialization. Drain workers before close, structural edits or external replacement.

Concurrent numerical work uses ordinary C++ memory rules. Backend capability flags and physical conflict units further constrain transfers: Zarr writes must exclude overlapping chunks or shards, and HDF5 thread safety does not imply parallel I/O. Streams serialize issuance/completion and retain conflict ownership; adaptive planning remains the default and creates no execution pool.

A failed transfer is not successful completion. Retry only when replay is safe, or drain other workers and abandon the stream. Completed earlier shards/objects may remain modified after a later error. There is no whole-store rollback or implicit durability guarantee. See [streaming](streaming.md) for request and metric contracts.

## Toolchains and compatibility

C++20 named modules, CMake >=3.28, Ninja >=1.11 and a matching Clang/dependency scanner are required by the initial toolchain. Linux x86-64 uses Clang 18. Native Apple AArch64 was checked with Clang/scanner 20.1.3 on macOS; its unavailable sanitizers, contended timing flags, incomplete consumer comparison, and resource-limit/reproducibility coverage limits are documented in the [ARM report](v1-arm-qualification.md). Those gaps were accepted under the two-machine release policy; they are not passing checks. POSIX file/mapping/spill mechanics and storage dependencies remain explicit platform boundaries.

The 1.x promise covers documented public source APIs and data contracts. Consumers build module interfaces for their compatible compiler, standard library and flags; precompiled BMIs and a stable binary ABI are not promised. Names in `detail` namespaces, private partitions and implementation headers are internal facilities. Pre-1.0 source changes and replacement spellings are documented in [migration notes](migration-1.0.md).
