# Format-family qualification

A subsequent independent review on 15 November 2026 reproduced five unresolved defects, including memory and destination-preservation failures. See the [review handoff](format-family-review-handoff.md). The passing matrix below remains a record of its tested cases; it does not establish full acceptance.

The format-family implementation passed the Linux checks below on 14 November 2026. The [machine-readable record](../benchmarks/results/format-family-qualification.json) contains the source-input hashes, toolchain and configuration identity, observed fixture hashes, and resolved reference versions. The [format guide](format-family.md) defines the supported API and encoding boundaries.

Qualification started from reviewed `origin/main` commit `e8c51ce7c3d8c2ded9d1b5f43c1e4d035ea13629` in an isolated worktree. The unchanged baseline passed 20 native/installed-consumer checks. No CCell, Cellerator, or downstream dependency pins were changed.

## Build and test matrix

The host was x86-64 Linux, with Clang and clang-scan-deps 18.1.3, CMake 3.28.3, Ninja 1.11.1, system HDF5 1.10.10, and Arrow/Parquet 21.0.0 for enabled builds.

| Configuration | Passing checks | Parquet |
| --- | ---: | --- |
| Debug | 33/33 | Off |
| Debug plus independent reference suites | 49/49 | On |
| Release | 34/34 | On |
| ASan/UBSan | 34/34 | On |
| Core-only, HDF5 and Zarr disabled | 20/20 | Off |
| HDF5-only | 28/28 | Off |
| Zarr-only | 26/26 | Off |
| Shared libraries and installed consumers | 34/34 | On |
| Focused ThreadSanitizer | 4/4 | Off |

Every complete native configuration includes installed consumers using both facade import spellings. The core consumer builds exactly five core BMIs without storage dependencies. Shared-library installation was checked with the private Arrow libraries outside system search paths. The installed library retains its configured private dependency search paths; consumers need the corresponding runtime libraries to remain available.

The 49-check configuration includes the existing AnnData reference checks and independent MuData, TreeData, AnnDataSet, SpatialData, and private Parquet adapter checks. The separate legacy MuData generation/native conversion/current-reader check also passed for HDF5 and Zarr. ThreadSanitizer covers worker-local transfer contexts and threaded Zarr v2/v3 streaming/completeness operations. It does not establish arbitrary shared-session thread safety.

ASan used `ASAN_OPTIONS=detect_leaks=0`, and UBSan used `UBSAN_OPTIONS=halt_on_error=1`; this is not leak qualification. The restricted runner used the existing test-only `ANNDATAPP_INTEROP_ZARR_WAKEUP=1` workaround. The usual completeness address-space limit remained active in the ordinary reference suite. These results do not qualify other platforms or compilers.

## Independent file checks

| Reference | Independently exercised behavior |
| --- | --- |
| MuData 0.4.1 and 0.3.10 | H5MU/Zarr native writes reopened upstream, all shared-axis modes, modality order and explicit maps, partial membership, global annotations and raw. Genuine older fixtures exercise absent-axis defaults and fixed-width Unicode Zarr arrays. |
| TreeData 0.4.0 and 0.2.5 | Current HDF5 JSON and columnar Zarr, historical scalar-JSON Zarr, all three alignments, ancestor projection and disconnected subsets, overlapping trees, raw axes, nested typed attributes and absence/null distinctions. Selected columnar reads avoid decoding discarded poisoned rows. |
| SnapATAC2 2.10.0 | Upstream H5ADS manifests and H5AD components read natively; native creation and copied/rebased manifests reopened upstream. Checks include relocation, common variables, sparse widths, extra manifest columns/attributes, source-byte preservation, and both missing and available Zstandard filter behavior. |
| SpatialData 0.9.0 | Current WKB and GeoArrow fixtures plus storage 0.2 and 0.3 fixtures written by separately pinned SpatialData 0.3.0 and 0.5.0. All five element types are reopened independently, including categories, IDs, table links, holes and multipolygons, circles, multiscales and coordinate transforms. An odd-sized pyramid independently verifies exact level scale and translation. |

The [reference environment instructions](../tests/reference/README.md), six lock files, and small generators are retained. Fixtures were generated in separate environments without upgrading the existing interoperability environment. The machine-readable report records observed byte hashes; HDF5 metadata can produce different byte hashes on regeneration. Temporary TreeData and AnnDataSet cases record their hashes before cleanup. Generated stores and environments are not distributed with the library.

Native regressions additionally check retained lifetimes, nested dataset roots and sibling isolation, raw-array prepared contexts retained beyond store owners, close/replacement invalidation, duplicate and reverse selections, dense/sparse caller buffers, manifest copy versus materialization, named-axis composition and singular inverses, strided geometry, valid dangling spatial references, disabled capabilities, and bounded validation/packing failures that preserve existing destinations.

## Memory, interface, and cost checks

A private edit to `src/spatialdata_transform.cpp` compiled only that implementation unit. All 15 public BMIs and both observed consumer objects retained their hashes and modification times. Restoring the source reproduced the original object, library, and executable bytes. The existing private-rebuild probe records this separately from numerical correctness.

Coordinate-transform allocation tests pass for fresh and repeated caller-buffer operations. Numerical coordinate loops use fixed storage and allocate nothing. Selected sparse export accounts for packed values, indices, offsets and temporary lookup storage together with backend chunk/shard workspace. MuData annotation export uses retained views; its allocation regression avoids copying the borrowed 8 MiB annotation payload. Spatial identifier and annotation-pair validation uses one checked row-index buffer and borrowed exact values, including uint64 and gathered/reversed rows.

Caller scratch budgets cover the documented library transfer, serialization, packing and validation staging. Retained metadata, explicitly returned owners/output buffers, and third-party internal allocations are separate. A selected noncontiguous sparse array must fit its packing budget; increase the budget or explicitly copy to a contiguous owner when it does not. This implementation does not claim a general streaming sparse exporter.

The [reproducible probe](../tools/format-family-probe/README.md) records these bounded observations in the machine-readable report:

| Observation | Measured cost and scope |
| --- | --- |
| Fresh installed consumer, `import anndata;` | 8.63 s including generation of 15 consumer-local module interfaces; the consumer translation unit itself took 0.05 s with its BMIs available. |
| Fresh installed consumer, `import anndatapp;` | 8.69 s including the same 15 interfaces; the consumer translation unit took 0.05 s. |
| One million borrowed FP64 coordinate pairs | Five affine calls took 7.36–7.60 ms, excluding caller buffer allocation; sampled output checks passed. |
| Retained raw HDF5 selection into strided caller memory | Five 4 MiB transfers took 8.36–10.11 ms (median 8.66 ms). Each reported 1,024 reads, 4 MiB read, 4 KiB retained scratch under a 64 KiB limit, and zero additional scratch allocations. All values and padding were checked. |

The two consumer builds ran sequentially with Ninja's default internal parallelism. Import and coordinate measurements preceded the private raw-geometry repair; the report verifies that all 15 public interface hashes remained unchanged. The transfer measurement used the repaired Release library. Normal OS/HDF5 caches remained active, without eviction or a controlled cold-cache claim. These fixed observations do not establish comparative performance or total third-party allocation bounds.

## Reproduction and retained evidence

Configure with Clang 18 and the private storage dependency prefix as described in [development](development.md). Enable the full reference suite with:

```sh
cmake --preset debug \
  -DANNDATAPP_DEPENDENCY_PREFIX=/path/to/storage/dependencies \
  -DANNDATAPP_ENABLE_PARQUET=ON \
  -DANNDATAPP_ARROW_ROOT=/path/to/arrow-or-pyarrow-prefix \
  -DANNDATAPP_ENABLE_INTEROP_TESTS=ON \
  -DANNDATAPP_ENABLE_FORMAT_INTEROP_TESTS=ON \
  -DPython3_EXECUTABLE=/path/to/full-reference/bin/python3 \
  -DANNDATAPP_TREEDATA_LEGACY_PYTHON=/path/to/legacy-tree/bin/python3 \
  -DANNDATAPP_SPATIAL_LEGACY02_STORE=/path/to/legacy02/fixture.zarr \
  -DANNDATAPP_SPATIAL_LEGACY03_STORE=/path/to/legacy03/fixture.zarr
cmake --build --preset debug --parallel 3
ctest --preset debug --parallel 3
```

Use the other existing presets for the matrix rows, passing explicit reference/dependency paths where needed. Parquet remains off by default. Generate legacy SpatialData fixtures with their locked interpreters before enabling their checks. For the separate MuData legacy check, use `tests/python_mudata.py` in `generate` mode with `ANNDATAPP_MUDATA_LEGACY=1`, convert each fixture with `test_mudata --io`, then use the current interpreter in `check` mode.

Raw build/test logs, full CTest output, private-rebuild receipts and fixture manifests are retained locally under `build/format-family-qualification-2026-11-14` after worktree closure. The compact tracked record identifies their source inputs. General write failure is not transactional: the tested known preflight failures preserve a destination, but a later codec or I/O failure can leave partial output. Unsupported Arrow extension columns, retired SpatialData 0.1, Zarr AnnDataSet, and unreleased nested TreeData export conventions remain explicit exclusions.
