# AnnData format family

This guide describes the native C++ models and the selected file-version scope. It records current source support, not interoperability results. The original feature qualification is recorded in [format-family-validation.md](format-family-validation.md); the 2026-11-15 repairs and their final matrix are tracked separately in [format-family-repair-validation.md](format-family-repair-validation.md).

The prepared-collection and gathered-metadata changes, bounded pre-extension comparisons, Python screens and initial raster selectivity finding are recorded in [format-family-io-validation.md](format-family-io-validation.md). Later corrections are qualified in [raster-selectivity-performance.md](raster-selectivity-performance.md) and [collection-preparation-fastpaths.md](collection-preparation-fastpaths.md).

The C++ facade can be imported with either `import anndata;` or `import anndatapp;`. Both export the same types in namespace `anndatapp`. The package, CMake target, and repository remain named `anndatapp`. The format modules can also be imported directly: `anndatapp.mudata`, `anndatapp.treedata`, `anndatapp.dataset`, and `anndatapp.spatialdata`.

The [native example](../examples/format_family.cpp) demonstrates construction and selection with `import anndata;`, plus optional backed inspection of an existing `.h5ads` manifest. CMake builds it as `anndatapp_format_family_example` when examples are enabled.

## File-version scope

| Format | Read and write scope |
| --- | --- |
| [MuData](https://mudata.scverse.org/stable/io/spec.html), reference 0.4.1 | Write current H5MU and Zarr files. Read 0.4.1 and legacy 0.3.10 files. Older files without axis metadata use the specified default shared axis. |
| [TreeData](https://treedata.readthedocs.io/en/latest/fileformat.html), reference 0.4.0 | Write current H5TD and Zarr files. Read current files and legacy 0.2.5 scalar-JSON Zarr fixtures. |
| [SnapATAC2 AnnDataSet](https://github.com/scverse/anndata-rs/blob/cf0b312f2834884bd708595a63f12f34f1905b7b/pyanndata/src/anndata/dataset.rs), reference 2.10.0 | Read and write HDF5 `.h5ads` manifests that link H5AD component files. The pinned implementation does not provide a Zarr collection dialect; its non-HDF5 construction path is unimplemented. |
| [SpatialData](https://github.com/scverse/spatialdata-spec/blob/storage-format/storage-format-versions.md), reference 0.9.0 | Read storage formats 0.2, 0.3, and 0.4; write 0.4. Retired storage format 0.1 and its ragged-shape encoding are excluded. |

H5MU, H5TD, and SpatialData use the existing HDF5 or Zarr storage implementations. AnnDataSet is an HDF5 collection manifest plus linked H5AD data. Do not infer compatibility with other generations or combinations from the in-memory models.

## Shared construction, selection, and storage

The models reuse AnnData's arrays, strings, columns, tables, and views. Large owners are move-only. Calling `copy()` explicitly creates independent data; a view retains its source binding and may represent a selection without making a contiguous numerical copy. Use the format's read/materialize function when an owning eager value is needed.

`Element::sequence()` represents an ordered heterogeneous sequence for native metadata. Generic AnnData serializers reject sequences when a format has no established sequence encoding. TreeData's JSON representation and store-attribute conversion provide explicit encodings. Missing attributes remain different from explicit null values.

`BackedStore` exposes groups, attributes, raw arrays, string arrays, and AnnData rooted at a group in an HDF5 or Zarr store. `create_array` and `create_strings` create backed objects; the ranged `write_strings(path, start, count, values)` updates a specified string-array region from a caller-owned batch. For example, `store.anndata("/tables/sample")` opens the AnnData dataset at that store path. Ordinary H5AD and Zarr entry points continue to operate at the store root. Acquired backed descriptors retain the physical session; explicitly closing the store invalidates them. Eager reads and caller-buffer transfers are explicit operations. Structural changes must follow the lifetime and transfer rules in [backed storage](storage.md) and [streaming](streaming.md).

`BackedArray::content_revision()` is a change-detection token for writes through the shared session. Compare tokens for equality to detect possible content changes; the value is not a write count or a success indicator. A write advances it before a transfer that can fail after partially changing storage, so a changed token means callers must treat the content as potentially changed and inspect or reread it.

For bounded array copies, `ArrayView::copy(validation, max_bytes)` limits the copied numerical values and index/offset buffers, including temporary sparse-selection staging; selection and shape metadata are excluded. `ArrayView::validate()` checks retained source buffers in place and does not materialize the selected result. `copy_buffer_bytes(validation, max_bytes)` accounts for result buffers and peak sparse-index staging without copying numerical values; temporary index buffers are released before it returns. A selected sparse copy packs the selected result and may need a mapping proportional to the selected minor axis. It fails when the packed result and required staging exceed the limit; this overload is not a streaming transfer. For dense destinations, `copy_into` writes into caller-owned storage without allocating a numerical destination.

## MuData

`MuData` contains ordered AnnData modalities, global annotations, a shared-axis mode, and explicit observation and variable maps. Construct an empty container with `MuData::empty`, then add modalities with `set_modality`. Each map has one entry per global axis position: zero means that the modality has no member at that position, and positive values are one-based modality positions. The map must cover the modality axis. `modality_names()` returns the stored order.

Global annotations live in `annotations()`. They hold axis annotations; X, layers, and raw belong to the modalities. Map updates are explicit and no hidden annotation synchronization is performed.

`slice` applies global observation and variable selections through each map. It preserves modality order, missing membership, selection order, and repeated selections. A backed `open_h5mu` or `open_mudata_zarr` view or positional slice retains descriptors without reading map or modality-matrix payloads. The first access to a modality or one of its maps reads the required maps; each modality-axis map is cached for that view, and a returned `MuMapView` pointer remains valid while its originating view lives. Map-content or modality-structure changes make captured views stale and subsequent view operations fail. Label selectors may read axis names. Call `read()` to materialize the selected MuData value. Write with `write_h5mu` or `write_mudata_zarr`.

## TreeData

`TreeData` composes an AnnData value with named observation and variable trees. `Tree::from_edges` accepts packed node identifiers and source/target pairs indexed into that node list; `Tree::from_arrays` accepts the corresponding native string and uint64 arrays. Node and edge attributes are Elements, so nested and heterogeneous metadata can be retained without stringifying values.

Choose `TreeAlignment::leaves`, `TreeAlignment::nodes`, or `TreeAlignment::subset` when constructing the model. Alignment controls which node IDs correspond to rows or variables. Set trees explicitly with `set_obs_tree` and `set_var_tree`; validation checks the alignment and configured overlap policy. A label column can be maintained by the model when a label is configured.

Tree selections project topology and retain relevant attributes. In leaves alignment, selecting rows keeps the matched leaves and their ancestors. In nodes alignment, selected axis IDs determine the retained nodes; a disconnected result is represented as a subset. Subset alignment allows topology beyond the selected AnnData axis. Use `TreeData::copy` for independent data. Backed readers are `open_h5td` and `open_treedata_zarr`; writes are `write_h5td` and `write_treedata_zarr`. Pass `TreeWriteOptions` to either writer to set `scratch_bytes` (64 MiB by default); it bounds accounted library-owned serialization and transfer staging. Source tree data remains caller-owned, and an indivisible value that cannot fit is rejected.

For HDF5 and legacy scalar-JSON Zarr tree collections, `tree_names()` scans the indivisible JSON scalar but does not construct every tree. A keyed tree request decodes and constructs only that member, without retaining a decoded-tree cache; the next request scans and decodes again. Grouped Zarr reads can address one tree directly. Unknown child groups in that representation are rejected during tree reads rather than silently omitted.

## AnnDataSet

`AnnDataSet` links an ordered list of H5AD components through an HDF5 `.h5ads` manifest. Open an existing manifest with `open_h5ads` or create one with `create_h5ads`. Relative component paths resolve from the manifest's parent. Open-time relocation overrides replace selected component paths in memory without rewriting the manifest. Missing or incompatible component files produce contextual errors.

`view()` and `slice()` keep the collection backed. `AnnDataSetView::prepare_x()` returns a move-only `PreparedAnnDataSetRead`: preparation checks the selected observation and variable names, records the exact stacked requirements, and retains per-piece `PreparedRead`s while their accounted descriptors fit the planning budget. If they do not fit, transfer rebuilds bounded pieces one at a time. Use the prepared object's `requirements()`, `preflight(DenseBuffer)` or `preflight(SparseBuffer)`, and matching `read_into` overloads to reuse a plan. `read_into` preflights every selected piece before writing the destination, so preflight failures leave caller storage untouched; later I/O failures can still leave a partial transfer.

The planning allowance counts retained descriptor payload/capacities and variable-sized selector/offset staging. One temporary candidate's fixed descriptor headers during acquisition, allocator/control-block overhead, shared source/session owners and caller buffers are outside that allowance; it is not a total allocation-peak or RSS limit.

The existing `AnnDataSetView::requirements()` and `read_x_into(...)` methods remain convenience wrappers that create and discard a prepared plan on each call. Calling `requirements()` and then a one-shot `read_x_into(...)` therefore prepares twice. `materialize()` explicitly builds an owning AnnData value and prepares X once when it is present. Collection annotations are stored separately from component data; reserved collection structure is not replaceable through the annotation setter.

A prepared plan retains the collection's backing sessions and selected descriptors, but it is not a coherent snapshot or a thread-safety mechanism. `AnnDataSet::close()` or a manifest structural edit invalidates it; `set_annotation()` also invalidates existing plans, even for an unrelated annotation. Linked components are read-only through the collection; synchronize writes through independent sessions or processes with preparation and transfer. No thread-safety or concurrent-writer consistency is promised.

Opening reads the manifest references and opens every linked component, checking component presence, axis sizes, and consistent X presence without comparing all axis names. `validate()` explicitly compares complete observation and variable indexes; `prepare_x()` compares the selected axis names needed by that transfer, and label selection can read names. Creation requires matching component variable names and consistent X presence; observation data are concatenated in component order.

Preparation reuses freshly checked metadata within the operation and compares bounded manifest observation-name batches against component segments. It retains one reference batch and one peer, with the existing short-prefix and tight-budget fallbacks; metadata is not cached across operations. Numerical reads and mmap/direct-pointer paths retain their existing contracts.

`copy_manifest()` copies the manifest and rebases its component references so they continue to resolve to the existing target files. It does not copy or materialize component data. Relocate or copy data files separately when independent component storage is required.

## SpatialData

`SpatialData` stores named images, labels, points, shapes, and AnnData tables.

- Images and labels use raster arrays and can hold multiple levels, channel metadata, named coordinate transforms, and a transform for each pyramid level. Each level transform is an axis-aligned positive scale plus translation, stored and written exactly. `add_level` chooses an extent-ratio scale by default; set `set_level_transform(level, transform)` when the specification needs an explicit scale or offset, such as an odd-sized pyramid. Raster views can select windows, which compose the crop with the selected level transform. Raster level arrays remain ordinary AnnData++ arrays.
- Points use a table plus a separate instance-identifier column. Feature keys, instance keys, axes, and transforms are explicit. Backed point reads accept a row range for bounded batches.
- Shapes represent circles with coordinate and radius arrays, and polygons with coordinate and ring/polygon/geometry offset arrays. IDs and attribute tables remain separate from the coordinate buffers. Selected shape views map logical rows to retained source topology; an explicit copy repacks the selected geometry.
- AnnData tables can carry region and instance-key links through `SpatialAnnotation`. The identifiers and table links remain element-local and are independent of coordinate transformations.

Full validation requires shape instance IDs to be non-null integer or string values and unique. When table links are present, each (region, instance) pair must be unique within that table; valid dangling references are allowed after filtering. Each uniqueness check uses one row-index array proportional to its row count, bounded by `scratch_bytes` when a limit is supplied. Direct `validate(scratch_bytes)` defaults to an unlimited workspace; spatial read/write options pass their configured limit to full validation. Owning SpatialData validation delegates through its view, so owner and view validation share the same checks, including nested AnnData table validation. Polygon construction checks offset shape, dtype, count, and sentinel chain before inferring geometry types. Numeric `ColumnView::source_values()` and `source_row()` expose the retained values and logical-to-source row mapping for borrowed comparisons without copying the selected values.

A `SpatialTransform` describes a map between named axes and supports identity, axis mapping, translation, scaling, and affine transforms. Transforms compose by matching intermediate axis names, can be inverted when nonsingular, and apply to borrowed input and caller-provided output buffers. The numerical coordinate loop allocates nothing; input and output must not overlap. Raster resampling is not included.

Use `SpatialDataView::select` to choose named elements and row-selection views on points or shapes to choose vector instances. `open_spatialdata` provides backed raster arrays and tables, bounded point reads, and ranged shape reads with `read_shapes(name, first, count)`. Shape geometry kind comes from GeoParquet metadata when present, so an empty or later-range read does not need to inspect an unrelated first geometry. Older WKB metadata without a geometry type still requires inspecting its first row to infer the kind. `read_spatialdata` eagerly reads the store; `write_spatialdata` writes the current format.

### Optional vector-file support

Arrow/Parquet is private and optional, disabled by default. Enable it with `ANNDATAPP_ENABLE_PARQUET=ON`; set `ANNDATAPP_ARROW_ROOT` to a supported Arrow/Parquet C++ installation or a PyArrow wheel prefix when CMake cannot discover it. This adapter carries bounded point batches and GeoParquet geometry payloads, including WKB and the supported native geometry encodings. Sidecar tables use boolean and fixed-width numeric scalars, UTF-8 strings, and categorical columns. Native categorical index widths are unchanged; categorical codes are normalized to Arrow int64 when written. Outside recognized geometry payloads, binary columns and Arrow timestamp, decimal, list/struct, and extension-typed columns are rejected with contextual errors. No Arrow types appear in the public modules.

Native points and shapes can still be constructed and selected without Arrow/Parquet. Reading or writing their spatial-file payloads requires the adapter. Operations that need an unavailable vector payload fail explicitly, and writes preflight this known capability before modifying the destination. `spatial_parquet_available()` reports whether the adapter is enabled. HDF5 files may also require filter plugins; a missing filter produces an explicit capability diagnostic. See [H5AD file behavior](h5ad.md) and [storage dependencies](storage-dependencies.md).

## Scratch limits and write failures

`TreeWriteOptions`, `SpatialReadOptions`, and `SpatialWriteOptions` default to 64 MiB of `scratch_bytes`. Backed spatial reads use the `BackedOptions` limit. These budgets cap explicitly accounted library staging and validation index workspaces; they do not cap process memory. Caller-owned source/output buffers and final returned values are outside the budget, as are third-party Arrow/Parquet, HDF5, and codec internal allocations. The Parquet adapter sizes its own batches using the limit, but allocations made inside Arrow/Parquet are not bounded by it. `parquet_row_group_rows` (65,536 by default) is an additional upper bound on write batch rows.

Known failures such as an unavailable Parquet capability, failed full validation, or a known budget/option violation are checked before a SpatialData destination is created or truncated. Set `SpatialWriteOptions::exclusive` to refuse replacing an existing store; the default mode truncates it. This preflight protects against those known failures, not all write failures: later serialization, codec, or I/O errors can leave a partially written store. TreeData writers likewise preflight known scratch requirements but do not provide whole-file rollback.

## Interoperability checks

The independent format reference checks use the CMake option `ANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS`, separate from the AnnData option `ANNDATAPP_ENABLE_INTEROP_TESTS`. Enabling a check does not itself establish a passing compatibility result; completed checks and fixture hashes belong in the qualification record.

## Explicit exclusions

This C++ API is native to AnnData++; it does not claim general Python API parity or provide scientific analysis pipelines. It includes coordinate transformations but not raster resampling. It does not invent a Zarr AnnDataSet format or encode unreleased nested MuData/TreeData conventions. Unknown or unsupported nested values are rejected rather than silently flattened. Unsupported nested TreeData materialization or export is rejected; same-store retention of untouched data remains governed by the backed-storage contract.

Other AnnData encodings, remote stores, arbitrary opaque cross-store conversion, and spatial encodings outside the selected version scope remain unsupported unless documented separately.
