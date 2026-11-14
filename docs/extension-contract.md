# Extending the package

The foundation is a native data library: arrays, tables, records, nested
elements, AnnData alignment, rooted backed access, and explicit transfers. The
current source includes MuData, TreeData, AnnDataSet, and SpatialData modules;
their APIs, selected wire-version bounds, and limitations are summarized in
[the format-family guide](format-family.md). Source presence does not substitute
for independent compatibility evidence.

## Logical data and memory

AnnData keeps two annotated axes; generic dense arrays remain N-dimensional.
CSR/CSC retain orientation, independent integer widths, duplicates and explicit
zeros. Preserve labels and ordering, null distinctions, categories, `.raw` and
unknown-data diagnostics before adding convenience accessors.

Owners are move-only and explicit `copy()` allocates independent data. Retained
views keep their allocation alive; borrowing transfers no lifetime obligation.
Direct acquisition exposes actual pointers and strides. A gather, compressed
chunk, or remote object is not contiguous memory merely because it has a shape.

`Element::sequence()` represents heterogeneous ordered values for native
metadata. It is distinct from mappings, arrays, columns, and tables; absent
attributes also remain distinct from explicit null. Generic AnnData serializers
reject sequences when no established encoding exists. TreeData JSON and
store-attribute conversions encode them explicitly.

`ArrayView::copy_into(ArrayEdit, Validation)` copies dense numerical selections
into existing storage without allocating a numerical destination or casting
values. Source and destination must not overlap. Metadata acquisition may
allocate; subsequent numerical loops perform ordinary loads and stores. Use
explicit `copy()` where overlap requires independent data.

The `ArrayView::copy(validation, max_bytes)` overload bounds its output
numerical/index/offset buffers and temporary sparse-selection staging;
selection and shape metadata are excluded. Selected sparse copies pack the
requested result and can fail when output plus mapping workspace exceeds the
limit. They do not stream the selection in pieces.

## Formats and backends

Keep HDF5, Zarr, JSON, Arrow/Parquet and codec headers behind compiled implementation boundaries.
Core types do not depend on Python, analysis frameworks, storage libraries, or
downstream applications. New named format modules may import the core/backed
API; internal partitions remain implementation components, not public modules.

`BackedStore` owns an existing physical session and exposes generic groups,
attributes, arrays, strings, and AnnData rooted at a group. Ordinary H5AD and
Zarr entry points use the store root; composed formats can use nested roots.
Keep backend types private and preserve descriptor retention, explicit close,
worker-local transfer contexts, scratch accounting, and physical chunk/shard
conflict rules.

Use established encoding names and versions. Pin the upstream specification and
reference implementation before writing a codec, independently generate input,
and independently reopen C++ output. Extend the existing element codecs where
possible; do not create a second numerical/table model for each container.

The private backed contract handles native-endian numerical transfers and
backend capabilities. File byte order remains separate from native access.
Keep format relationships in their owning models and use rooted datasets for
nested AnnData; do not generalize every core type or add a storage plugin
framework. Domain conventions such as Scanpy results normally need typed
accessors or validation, not another storage engine.

Untouched extension data can remain in its original store. Unsupported encodings
and conversions must produce contextual errors rather than losing fields or
stringifying them. The eager model and cross-backend export do not promise
arbitrary opaque attribute preservation; see [H5AD](h5ad.md) and
[backed storage](storage.md).

## Transfers and execution

Callers own scheduling, synchronization and output storage. One transfer context
belongs to each worker. Structural mutation and explicit close follow existing
descriptor-invalidation rules. Native loops on acquired buffers never acquire
library locks or invoke type-erased accessors per value.

Physical chunks and shards define conflict units independently of logical
selections. Numerical writes complete within their existing calls; there is no
implicit cross-call transaction or background flush. A shard replacement is not
a whole-dataset transaction or a durability guarantee.

Charge library-owned grouping, index, packing and codec staging to the declared
scratch budget. Keep caller buffers and third-party internal allocations
separately identified. Raise an explicit capacity diagnostic when indivisible
work cannot fit; do not silently materialize a full dataset.

## Qualification

New adapters must pass native tests without Python where applicable, independent
format round trips, affected sanitizer/concurrency tests, and installed-consumer
checks. Preserve core-only and optional-backend configurations. CMake installs
module sources and metadata; compatible consumers build their own BMIs.

Keep public interfaces small and compiled implementations private. Record import
costs when changing interfaces and prove that private edits leave public BMIs and
consumer objects untouched. Start performance work with bounded, representative
geometry and call/byte/allocation counts; report exact source and fixture identity.

Current qualification targets and completed checks are recorded in the
qualification documents. The format-family validation record will list pinned
fixtures, environment identities, hashes, and outcomes independently of this
design contract. Unsupported encodings and the exclusions in
[format-family.md](format-family.md) remain explicit; do not infer coverage from
API availability.
