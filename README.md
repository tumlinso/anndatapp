# AnnData++

AnnData++ is an independently developed C++20 implementation of the AnnData data model. It is a separate project from the Python AnnData package. The additional C++ module spelling, `import anndata;`, does not change the project's ownership or status. No affiliation with or endorsement by the AnnData or scverse maintainers is implied.

A standalone C++20 library for annotated biological data, with named modules and direct control of numerical memory.

The [Python AnnData project](https://github.com/scverse/anndata) defines the data model and [storage conventions](https://anndata.readthedocs.io/en/stable/fileformat-prose.html) followed here.

The library provides **in-memory and backed AnnData, MuData, TreeData, linked AnnDataSet collections, and SpatialData**, with H5AD, H5MU, H5TD, Zarr, and HDF5-linked collection storage. Its native data includes dense arrays, CSR/CSC, heterogeneous metadata, and annotated matrices. Backed access reads selections into owned or caller-provided buffers and supports explicit structural updates. Python, Cellerator, and Torch are not runtime dependencies. Arrow/Parquet is optional and used for SpatialData vector-file payloads; a core-only build requires no storage backend.

## Parallel streaming reads

AnnData++ complements Python AnnData and the scverse ecosystem in native pipelines
that process backed data in batches. Prepare a `TransferContext` once per worker
and reuse that worker's output buffers. Schedule shard-aligned blocks for sharded
Zarr and consecutive whole rows for CSR H5AD. Callers own scheduling, so they can
pass each block directly to their numerical code and bound memory by worker count
and block size. [Streaming guide](docs/streaming.md)

On Linux x86-64 with four workers and warm caches, AnnData++ delivered
**6.4× the throughput of Python Zarr** on shard-aligned Zarr reads and
**23× the throughput of Python AnnData's backed sparse reader** on CSR H5AD
row blocks. Both readers used the same files, block requests, and CPU allocation.

| Backed streaming workload | AnnData++ | Python reference |
| --- | ---: | ---: |
| Zstd-sharded Zarr v3, 256 × 512 shard blocks | 510.5 MiB/s | 80.3 MiB/s |
| Gzip CSR H5AD, 64-row blocks | 4,216.5 MiB/s | 182.3 MiB/s |

These are median logical throughputs for verified 4096 × 1024 fixtures with
worker setup measured separately. Python Zarr 3.4.2 reads into reusable buffers.
For H5AD, both readers use persistent processes. AnnData++ reuses caller
buffers and Python AnnData 0.13.4 with h5py 3.16.0 returns a SciPy CSR block
per request. The measurements cover backed streaming on these layouts.
[Method and reproduction](benchmarks/parallel_streaming/README.md)

## In-memory example

```cmake
find_package(anndatapp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE anndatapp::anndatapp)
```

```cpp
import anndata;

int main() {
    auto data = anndatapp::AnnData::empty(3, 2);
    data.set_x(anndatapp::Array::empty<float>({3, 2}));
    auto x = data.edit_x().dense<float>();
    float* p = x.data();
    for (int i = 0; i < 6; ++i)
        p[i] = static_cast<float>(i);
    auto selected = data.slice(anndatapp::indices{0, 2}, anndatapp::all);
    auto independent = selected.copy();
    anndatapp::write_h5ad("selected.h5ad", independent);
    auto loaded = anndatapp::read_h5ad("selected.h5ad");
}
```

Numerical storage supports booleans, fixed-width integers, FP32, and FP64. Dense arrays also support opt-in IEEE binary16 storage through `anndatapp::Float16`, including H5AD and Zarr v2/v3. CSR and CSC use the existing dtype set. See [dense FP16 storage](docs/fp16.md) for the native and file contracts.

Owners are move-only. `copy()` creates independent data. Typed memory views retain storage by default and expose pointers, dimensions, and strides. Borrowed and unchecked paths give callers explicit control over lifetimes and validation. Parallel execution and synchronization belong to the caller.

[Reusable block I/O](docs/streaming.md) exposes physical chunk/shard geometry,
worker-local transfer contexts, and caller-executed adaptive or manual streams.
Runtime tuning uses measured transfers and explicit memory/concurrency limits;
it contains no device labels or background execution pool.

## Build

The initially qualified toolchain is Linux, Clang 18 with matching `clang-scan-deps`, CMake 3.28 or newer, and Ninja 1.11 or newer. Default builds use HDF5, zlib, Zstandard, C-Blosc and private JSON parsing. [Dependency commands](docs/storage-dependencies.md) include system and private-prefix installation.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The native test suite needs no Python. `ANNDATAPP_ENABLE_INTEROP_TESTS` enables the AnnData reference checks; `ANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS` enables the separate pinned format-family checks. See the [H5AD encoding subset](docs/h5ad.md), [backed storage contracts](docs/storage.md), and [format-family guide](docs/format-family.md).

Use `cmake --workflow --preset core-only` for a dependency-free build. `hdf5-only` and `zarr-only` qualify single-backend packages. The default facade imports `anndata` or `anndatapp`; both export the same `anndatapp` C++ types. `import anndatapp.core;` remains the smallest in-memory interface, while `anndatapp.backed`, `anndatapp.streaming`, `anndatapp.h5ad`, `anndatapp.zarr`, `anndatapp.mudata`, `anndatapp.treedata`, `anndatapp.dataset`, and `anndatapp.spatialdata` are the named modules.

Consumers can use `add_subdirectory()` or install the package and link `anndatapp::anndatapp` through `find_package(anndatapp CONFIG REQUIRED)`. The independently importable core is `import anndatapp.core;`. Both `import anndatapp;` and `import anndata;` are convenience facades; the package, targets, and C++ namespace remain `anndatapp`. Module sources and CMake metadata are installed so consumers build BMIs with their own compatible toolchain. There is no public umbrella header and no requirement for `import std;`.

## Design and evidence

- [Architecture and compatibility matrix](docs/architecture.md)
- [AnnData format family APIs and file-version scope](docs/format-family.md)
- [Format-family C++ example](examples/format_family.cpp)
- [Thin-library review and implementation backlog](docs/review/README.md)
- [1.0 cleanup status](docs/v1-status.md), [support contract](docs/support.md), and [migration notes](docs/migration-1.0.md)
- [1.1.0 Linux qualification](docs/v1.1-linux-qualification.md)
- [1.0.0 release notes](docs/release-notes-1.0.md) and [final Linux qualification](docs/v1-release-linux.md)
- [Bounded standard performance suite](docs/v1-performance.md)
- [Extension contracts and qualification requirements](docs/extension-contract.md)
- [Development, installation, and validation](docs/development.md)
- [Naming cutover validation](docs/rename-validation.md)
- [Package boundary validation](docs/package-boundary-validation.md)
- [Memory access and ownership](docs/memory.md)
- [Native records and compound H5AD elements](docs/records.md)
- [Compact selection and direct metadata validation](docs/selection-validation.md)
- [H5AD encodings and file access](docs/h5ad.md)
- [Backed storage, structural edits and mappings](docs/storage.md)
- [Reusable transfers, parallel block streams and runtime tuning](docs/streaming.md)
- [Storage dependency installation commands](docs/storage-dependencies.md)
- [Validation results and tested reference versions](docs/validation.md)
- [Measured performance and compilation behavior](docs/performance.md)
- [H5AD I/O and optional-module measurements](docs/h5ad-performance.md)
- [Backed storage and compilation measurements](docs/storage-performance.md)
- [Shard batching and bounded runtime-tuning measurements](docs/completeness-performance.md)
- [Matched Python and C++ runtime and memory comparisons](docs/head-to-head-performance.md)
- [Gathered Zarr read correction and paired measurements](docs/gather-read-performance.md)

AnnData and Scanpy conventions determine data alignment and biological metadata behavior. The C++ interface uses native ownership, pointers, explicit copies, and lightweight typed access rather than reproducing Python mechanics.

Licensed under the [MIT license](LICENSE).
