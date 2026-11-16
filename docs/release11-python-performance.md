# Raster and collection comparison for 1.1, 16 November 2026

The raster paths are good on the tested windows: they fetch exactly the required chunks and outperform both SpatialData's first-call public API and the separate direct-Zarr controls. Collections remain mixed. Their native X-only task is materially slower than SnapATAC2; they are competitive with an auxiliary Python task that also checks selected axis names. That auxiliary result does not erase the native API gap.

This is performance evidence for a release decision, not complete release qualification or publication. The library is frozen at `180c440ad4115306ede27bc2e86f927bee4acf94`; no production implementation, public API, dependency version or downstream project changed. The [machine record](../benchmarks/results/release11-python-performance.json) contains individual samples, medians, counters and receipt hashes.

## Same requested output

Both sides read the same immutable files and return fully materialized, contiguous uint16 rasters or float32 CSR matrices. Every value, sparse index and offset is checked against an independent formula, and all implementations and samples match canonical output digests. Hashing, normalization and correctness checks occur after the operation clock. No sorting, duplicate reduction or unnecessary dense conversion is added to Python.

SpatialData 0.9.0 selects the image and level before `isel(...).data.compute(scheduler="synchronous")`. AnnData++ selects the same backed raster and reads into an allocated caller buffer. The separate storage controls use AnnData++'s raw store array and Zarr 3.4.2 `oindex`, both targeting the agreed level-0 `images/image/s0` path. This known path is verified from fixture metadata before the campaign; Python does not perform additional format discovery in its raw control.

SnapATAC2 2.10.0 uses simultaneous `data.X[rows, cols]`, avoiding the earlier selected-rows/all-columns intermediate. AnnData++ uses one `prepare_x()`, requirements, caller-buffer allocation and `read_into()`. Both select 64 rows and 64 variables across component boundaries and return the same CSR entries. The small output has 512 nonzeros; the 10k output has 17. Small outputs use int32 indices/offsets on both sides. AnnData++ retains int64 offsets for the 10k source; Python returns int32. That difference is disclosed; canonical conversion is outside timing.

The auxiliary Python row adds selected observation/variable-name comparisons against the manifest, using bounded h5py index selections and no X payload. Its extra HDF5 resource opens and manifest reads are timed. It is an explicitly written adapter, not an upstream SnapATAC2 option, and it does not reproduce every C++ sparse-validation or caller-buffer guarantee. The native API result remains the primary X-only comparison.

## Raster results

Times are milliseconds from opening the store through materialized output, excluding imports and verification. Primary rows are medians of three fresh-process paired observations. Raw rows have one paired observation each and support a limited storage control, not a stable speedup estimate.

| Raster window | AnnData++ public | SpatialData public | AnnData++ raw | Zarr raw |
|---|---:|---:|---:|---:|
| Existing decimated | 5.45 | 290.48 | 2.09 | 3.42 |
| Existing contiguous | 5.42 | 292.12 | 1.77 | 3.70 |
| Four of sixteen chunks | 2.20 | 280.30 | 1.40 | 4.63 |

The existing image is shape `[1,256,256]`, with one `[1,256,256]` uint16/Zstd chunk. Decimation selects rows `16:80:2`, columns `32:96:2`; the contiguous control selects `16:48` and `32:64`. Both return `[1,32,32]`, or 2,048 bytes. Every implementation fetches the same 131,084-byte chunk once. Decoding a full chunk remains necessary for this layout.

The additional 532,326-byte store has shape `[1,512,512]`, sixteen `[1,128,128]` chunks and the same dtype/codec family. Rows and columns `96:224:4` return the same output shape. All four implementations fetch precisely chunks `(0,0)`, `(0,1)`, `(1,0)`, `(1,1)`, once each, at 32,778 encoded bytes each. None of the other twelve chunk payloads is observed. These are path-specific read observations; this observer does not count decoder frame completions.

The large SpatialData difference includes first-use lazy setup and graph/format handling after imports. It is not evidence that C++ decompression is tens of times faster. The direct-Zarr controls are the stronger check against an efficient storage baseline. They also favor AnnData++, while requiring the same chunks and output. Acquisition still has room to improve: the decimated public C++ call requests 21,147 observed C++ allocations / 2,264,892 cumulative bytes, versus 5,733 / 1,104,384 for its raw control. These are not live-memory figures or cross-language allocation comparisons.

**Verdict:** good selected raster I/O on these layouts, beyond fixing the previous repeated-chunk failure. This does not establish cold-cache throughput, sustained large-raster performance or minimal metadata overhead.

## Collection results

Each row is a three-sample median in milliseconds, including open, selection, planning/checking, output allocation and materialization.

| Collection | AnnData++ | SnapATAC2 X-only | Python plus selected-name checks |
|---|---:|---:|---:|
| Two components, 256 variables | 15.95 | 5.93 | 14.79 |
| Eight components, 10k rows × 20k variables | 60.49 | 39.01 | 68.00 |

AnnData++ is **2.69× slower** for the small native task and **1.55× slower** for the 10k native task. Against the auxiliary selected-name-checking task it is within the screen's materiality threshold for the small case and about 11% faster for 10k. Because that adapter reopens HDF5 resources and the validation contracts still differ, this is context about the cost of stronger checks, not an unconditional Python speed win.

Preparation remains the main C++ cost. Its median is 7.54 ms for the small case and 39.35 ms for 10k; those phases combine selected-axis checking and sparse sizing. Transfer medians are 1.33 and 5.61 ms, while Python's full post-open indexed operations take 1.08 and 6.55 ms. Those phases perform different work and have different cache histories; their ratios are not equivalent-kernel speedups. Medians from different phases need not sum exactly to the end-to-end median.

Separate diagnostic processes observed:

| Collection and implementation | Intercepted read calls | Intercepted bytes |
|---|---:|---:|
| Small C++ | 139 | 139,739 |
| Small Python native | 124 | 119,361 |
| Small Python with name checks | 222 | 209,002 |
| 10k C++ | 718 | 16,531,840 |
| 10k Python native | 413 | 10,636,688 |
| 10k Python with name checks | 922 | 20,414,744 |

The 10k C++ preparation accounts for 327 calls / 11,214,392 bytes, and transfer for 72 / 4,266,904. Its source numeric arrays are contiguous, uncompressed HDF5 datasets. The small C++ case retains the previously qualified scan-local handle reuse: preparation observes 25 reads / 39,630 bytes and transfer 6 / 17,416. The new observer does not count logical HDF5 calls or object opens. These numbers do not identify physical device traffic or establish that collection byte access is optimal. The implementation remains a bounded selected scan rather than full-matrix materialization.

**Verdict:** not an unqualified high-performance win for X-only collection access. It is competitive when selected-axis consistency checks are part of the requested task, but preparation and the lean native-API gap remain material. Any next investigation should separate 10k name checking from sparse sizing using the existing phase probe; no such deeper campaign or repair was performed here.

## Conditions and recovery evidence

All **60 process records** completed and passed. The raster window took 25.59 seconds, collection 24.16 and diagnostics 17.78: **67.54 seconds of measurement windows**, within three separate 30-second caps. First start to final completion spanned 92.59 seconds including orchestration gaps. No additional timing windows, larger fixtures or 100k-row runs were started. Both primary comparisons have three observations; raw controls have one. A material flag requires both a 10% and 0.1 ms difference. These small samples do not establish statistical certainty.

Runs were serial on CPU 0 with six recorded thread limits of one, synchronous Dask computation and Zarr async concurrency/max workers set to one. Python used its normal event loop with `ANNDATAPP_INTEROP_ZARR_WAKEUP=0`; the workaround was used only for fixture-generation/semantic smokes. Normal OS/backend caches were not evicted. Input hashing and previous work can warm them; external host activity was not controlled. Imports/setup are reported separately, while first-use work inside a reader remains timed.

Native HDF5 is 1.10.10 and Zstd 1.5.5. The auxiliary h5py 3.16.0 reader uses HDF5 2.0.0. SnapATAC2 bundles native code and does not expose its embedded HDF5 version through the inspected public API. These are package/backend comparisons, not language attribution. Python reader initialization, allocation and C++ destination preflight differ; end-to-end requested-output comparisons are the meaningful headline.

The libc-only interposer deliberately avoids the earlier Python HDF5-symbol failure. It counts intercepted read/pread/readv and write counterparts, not all syscalls, mmap page-ins or physical I/O. C++ `new` counters omit Python, NumPy and Rust allocators; zero observed requests in Python is not zero allocation. Instrumented runtimes are excluded from timing results.

Reproducible probes are in [release11](../tools/format-family-screen/release11/README.md). Persistent raw evidence, source/binary/fixture identities, a source archive, consumed static-library prefix and checkpoint are under `build/release11-performance-2026-11-16/` in the managed worktree. Existing reference environments, fixtures, baseline prefixes and earlier reports were preserved. No universal no-regression claim, broader release acceptance or release publication follows from this screen.
