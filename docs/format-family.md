# AnnData format family

This guide describes the native C++ models and the selected file-version scope. It records current source support, not interoperability results. Qualification evidence, resolved reference environments, fixture identities, and outcomes are tracked separately in `docs/format-family-validation.md`.

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

For bounded array copies, `ArrayView::copy(validation, max_bytes)` limits the copied numerical values and index/offset buffers, including temporary sparse-selection staging; selection and shape metadata are excluded. A selected sparse copy packs the selected result and may need a mapping proportional to the selected minor axis. It fails when the packed result and required staging exceed the limit; this overload is not a streaming transfer. For dense destinations, `copy_into` writes into caller-owned storage without allocating a numerical destination.

## MuData

`MuData` contains ordered AnnData modalities, global annotations, a shared-axis mode, and explicit observation and variable maps. Construct an empty container with `MuData::empty`, then add modalities with `set_modality`. Each map has one entry per global axis position: zero means that the modality has no member at that position, and positive values are one-based modality positions. The map must cover the modality axis. `modality_names()` returns the stored order.

Global annotations live in `annotations()`. They hold axis annotations; X, layers, and raw belong to the modalities. Map updates are explicit and no hidden annotation synchronization is performed.

`slice` applies global observation and variable selections through each map. It preserves modality order, missing membership, selection order, and repeated selections. A backed `open_h5mu` or `open_mudata_zarr` can create a lazy view; acquiring the view reads its selected maps but not the modality matrices. Call `read()` to materialize the selected MuData value. Write with `write_h5mu` or `write_mudata_zarr`.

## TreeData

`TreeData` composes an AnnData value with named observation and variable trees. `Tree::from_edges` accepts packed node identifiers and source/target pairs indexed into that node list; `Tree::from_arrays` accepts the corresponding native string and uint64 arrays. Node and edge attributes are Elements, so nested and heterogeneous metadata can be retained without stringifying values.

Choose `TreeAlignment::leaves`, `TreeAlignment::nodes`, or `TreeAlignment::subset` when constructing the model. Alignment controls which node IDs correspond to rows or variables. Set trees explicitly with `set_obs_tree` and `set_var_tree`; validation checks the alignment and configured overlap policy. A label column can be maintained by the model when a label is configured.

Tree selections project topology and retain relevant attributes. In leaves alignment, selecting rows keeps the matched leaves and their ancestors. In nodes alignment, selected axis IDs determine the retained nodes; a disconnected result is represented as a subset. Subset alignment allows topology beyond the selected AnnData axis. Use `TreeData::copy` for independent data. Backed readers are `open_h5td` and `open_treedata_zarr`; writes are `write_h5td` and `write_treedata_zarr`. Pass `TreeWriteOptions` to either writer to set `scratch_bytes` (64 MiB by default); it bounds accounted library-owned serialization and transfer staging. Source tree data remains caller-owned, and an indivisible value that cannot fit is rejected.

## AnnDataSet

`AnnDataSet` links an ordered list of H5AD components through an HDF5 `.h5ads` manifest. Open an existing manifest with `open_h5ads` or create one with `create_h5ads`. Relative component paths resolve from the manifest's parent. Open-time relocation overrides replace selected component paths in memory without rewriting the manifest. Missing or incompatible component files produce contextual errors.

`view()` and `slice()` keep the collection backed. Collection-level selections can be transferred into an existing dense or sparse destination: ask `requirements()` for the stacked output layout, then call the corresponding `read_x_into`. `materialize()` explicitly builds an owning AnnData value. Collection annotations are stored separately from component data; reserved collection structure is not replaceable through the annotation setter.

Creation and validation require matching component variable names and consistent X presence. Observation data are concatenated in component order.

`copy_manifest()` copies the manifest and rebases its component references so they continue to resolve to the existing target files. It does not copy or materialize component data. Relocate or copy data files separately when independent component storage is required.

## SpatialData

`SpatialData` stores named images, labels, points, shapes, and AnnData tables.

- Images and labels use raster arrays and can hold multiple levels, channel metadata, named coordinate transforms, and a transform for each pyramid level. Each level transform is an axis-aligned positive scale plus translation, stored and written exactly. `add_level` chooses an extent-ratio scale by default; set `set_level_transform(level, transform)` when the specification needs an explicit scale or offset, such as an odd-sized pyramid. Raster views can select windows, which compose the crop with the selected level transform. Raster level arrays remain ordinary AnnData++ arrays.
- Points use a table plus a separate instance-identifier column. Feature keys, instance keys, axes, and transforms are explicit. Backed point reads accept a row range for bounded batches.
- Shapes represent circles with coordinate and radius arrays, and polygons with coordinate and ring/polygon/geometry offset arrays. IDs and attribute tables remain separate from the coordinate buffers. Selected shape views map logical rows to retained source topology; an explicit copy repacks the selected geometry.
- AnnData tables can carry region and instance-key links through `SpatialAnnotation`. The identifiers and table links remain element-local and are independent of coordinate transformations.

Full validation requires shape instance IDs to be non-null integer or string values and unique. When table links are present, each (region, instance) pair must be unique within that table; valid dangling references are allowed after filtering. Each uniqueness check uses one row-index array proportional to its row count, bounded by `scratch_bytes` when a limit is supplied. Direct `validate(scratch_bytes)` defaults to an unlimited workspace; spatial read/write options pass their configured limit to full validation. Numeric `ColumnView::source_values()` and `source_row()` expose the retained values and logical-to-source row mapping for borrowed comparisons without copying the selected values.

A `SpatialTransform` describes a map between named axes and supports identity, axis mapping, translation, scaling, and affine transforms. Transforms compose by matching intermediate axis names, can be inverted when nonsingular, and apply to borrowed input and caller-provided output buffers. The numerical coordinate loop allocates nothing; input and output must not overlap. Raster resampling is not included.

Use `SpatialDataView::select` to choose named elements and row-selection views on points or shapes to choose vector instances. `open_spatialdata` provides backed raster arrays and tables, bounded point reads, and ranged shape reads with `read_shapes(name, first, count)`. `read_spatialdata` eagerly reads the store; `write_spatialdata` writes the current format.

### Optional vector-file support

Arrow/Parquet is private and optional, disabled by default. Enable it with `ANNDATAPP_ENABLE_PARQUET=ON`; set `ANNDATAPP_ARROW_ROOT` to a supported Arrow/Parquet C++ installation or a PyArrow wheel prefix when CMake cannot discover it. This adapter carries bounded point batches and GeoParquet geometry payloads, including WKB and the supported native geometry encodings. Sidecar tables use boolean and fixed-width numeric scalars, UTF-8 strings, and categorical columns. Outside recognized geometry payloads, binary columns and Arrow timestamp, decimal, list/struct, and extension-typed columns are rejected with contextual errors. No Arrow types appear in the public modules.

Native points and shapes can still be constructed and selected without Arrow/Parquet. Reading or writing their spatial-file payloads requires the adapter. Operations that need an unavailable vector payload fail explicitly, and writes preflight this known capability before modifying the destination. `spatial_parquet_available()` reports whether the adapter is enabled. HDF5 files may also require filter plugins; a missing filter produces an explicit capability diagnostic. See [H5AD file behavior](h5ad.md) and [storage dependencies](storage-dependencies.md).

## Scratch limits and write failures

`TreeWriteOptions`, `SpatialReadOptions`, and `SpatialWriteOptions` default to 64 MiB of `scratch_bytes`. Backed spatial reads use the `BackedOptions` limit. These budgets cap explicitly accounted library staging and validation index workspaces; they do not cap process memory. Caller-owned source/output buffers and final returned values are outside the budget, as are third-party Arrow/Parquet, HDF5, and codec internal allocations. The Parquet adapter sizes its own batches using the limit, but allocations made inside Arrow/Parquet are not bounded by it. `parquet_row_group_rows` (65,536 by default) is an additional upper bound on write batch rows.

Known failures such as an unavailable Parquet capability, failed full validation, or a known budget/option violation are checked before a SpatialData destination is created or truncated. Set `SpatialWriteOptions::exclusive` to refuse replacing an existing store; the default mode truncates it. This preflight protects against those known failures, not all write failures: later serialization, codec, or I/O errors can leave a partially written store. TreeData writers likewise preflight known scratch requirements but do not provide whole-file rollback.

## Interoperability checks

The independent format reference checks use the CMake option `ANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS`, separate from the AnnData option `ANNDATAPP_ENABLE_INTEROP_TESTS`. Enabling a check does not itself establish a passing compatibility result; completed checks and fixture hashes belong in the qualification record.

## Explicit exclusions

This C++ API is native to AnnData++; it does not claim general Python API parity or provide scientific analysis pipelines. It includes coordinate transformations but not raster resampling. It does not invent a Zarr AnnDataSet format or encode unreleased nested MuData/TreeData conventions. Unknown or unsupported nested values are rejected rather than silently flattened. Unsupported nested TreeData materialization or export is rejected; same-store retention of untouched data remains governed by the backed-storage contract.

Other AnnData encodings, remote stores, arbitrary opaque cross-store conversion, and spatial encodings outside the selected version scope remain unsupported unless documented separately.
