# Collection preparation comparison, 2026-11-16

A single 12.654-second serial window compared the frozen Release baseline
library `180c440` with the candidate, and Python's native simultaneous
`X[rows, cols]` operation. Both C++ installations enabled HDF5, Zarr and
Parquet and used the same Clang 18 Release consumer and probe source. Normal
OS and HDF5 caches, one CPU and thread limits of one were used. Three fresh
processes were run for each version and workload; four additional C++
processes collected diagnostic counters. All 22 processes passed the
cellwise oracle and returned matching cross-language canonical FNV digests.

| Workload | Preparation baseline → candidate, ms | End-to-end baseline → candidate, ms | Python end-to-end, ms |
|---|---:|---:|---:|
| Small collection, 64 × 64, 512 nnz | 7.610 → 3.647 | 15.910 → 10.914 | 6.155 |
| 10k collection, 64 spread rows × 64 spread columns, 17 nnz | 38.794 → 25.383 | 58.499 → 44.938 | 38.645 |

Preparation samples, in execution order, were small baseline 7.258, 7.610,
7.665 ms; small candidate 3.447, 3.647, 3.802 ms; 10k baseline 38.794,
39.163, 38.496 ms; and 10k candidate 25.383, 25.666, 24.694 ms. End-to-end
samples were small baseline 17.244, 15.687, 15.910 ms; small candidate
10.425, 10.914, 11.470 ms; 10k baseline 58.499, 59.435, 58.341 ms; and
10k candidate 44.938, 45.392, 43.820 ms. No measured phase or end-to-end
median regressed by both more than 10% and 0.1 ms. The small transfer median
improved by 10.1% and 0.134 ms, just past that screen threshold; three samples
are insufficient to generalize that marginal result.

Python does not perform the selected-axis validation in the C++ contract, so
its timings are useful boundary context, not a like-for-like validation
comparison. The 10k C++ result uses int32 indices and int64 offsets; Python
uses int32 for both. Small results use int32 indices and offsets in all
versions. All values are float32 CSR. FNV digests are `bb7805bcc25ffdde`
(small) and `54073d4a0e9be58f` (10k).

Preparation-only diagnostic counters, baseline → candidate:

| Workload | H5Oopen | H5Dread (string memory type) | C++ new calls / requested bytes | Intercepted reads / bytes |
|---|---:|---:|---:|---:|
| Small | 244 → 105 | 135 (7) → 134 (6) | 2,980 / 122,616 → 1,295 / 64,066 | 25 / 39,630 → 24 / 39,032 |
| 10k | 790 → 273 | 153 (25) → 146 (18) | 9,786 / 400,448 → 3,923 / 193,090 | 327 / 11,214,392 → 322 / 10,931,400 |

These are interposed logical calls and requested bytes, not physical-device
I/O, live memory or total allocation. Interposed diagnostic durations are not
timing evidence. Candidate library SHA-256 is
`8122de5dd04065111433f305502bf408a960efbd8fcac1b8366b5a423dabaa38`;
baseline is `807e0551bc20c68d568c171387405de9b946f4b69520a7f02b7edae08f3ad66b`.
Both core archives are
`2b37603ef8c56078002abf05c5dd7ba2a295196ffe15bae5ed45d2554caeef19`.
Raw artifacts are retained locally under
`build/collection-prep-fix-2026-11-16/comparison/`: plan SHA-256
`a7848ed803578a0a44192a58bd09944637d259e0c7fc7fa2dbbedb8904d2f01b`,
receipt `e8bfd2fc79d96548c91c955623595f077d2f5252037633bf85fd612321560931`,
summary `02b9be52eb054d8bdbedc43f3505635d8ecf919473d5d202d2fbdd0503ca9e42`,
and pre-window identities
`ba1ffc81e030d21ecbd59c68704dfceb63ee0795a5781b0953a24ed5e288959e`.
All 34 identities matched at window start and summary verification. These
results do not establish cold-cache, scaling, build-time or broader workload
performance.
