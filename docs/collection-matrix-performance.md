# Broader collection benchmark, 17 November 2026

Collection performance is mixed. AnnData++ beats SnapATAC2 on the tested
one-component and contiguous reads, reaches roughly comparable first-read
latency for several eight-component workloads, and loses on component scaling,
interleaved duplicate selections and repeated prepared reads. Prepared plans and
caller buffers provide useful control, but do not deliver a speed advantage in
the two reuse cases measured here.

This compares the library at `32852b6b65d0ab6d8204f3cb5ebb7cc584be0def`
with SnapATAC2 2.10.0. It includes the new default positional preparation,
without cross-component name comparisons, and retains default structural/sparse
validation. No production code or dependency changed. It does not include any
subsequent small-collection fix or constitute complete release 1.1 qualification.
The [machine record](../benchmarks/results/collection-matrix-performance.json)
retains samples, phases, native widths, diagnostics and evidence hashes. The
[benchmark sources](../tools/format-family-screen/collection-matrix/README.md)
describe reproduction.

## First materialized read

Times are milliseconds from opening the collection through the first fully
materialized CSR output. Each row reports the median of `n` independent process
observations; `n=1` rows are screening controls, not stable latency estimates.

| Workload | AnnData++ | SnapATAC2 | n |
|---|---:|---:|---:|
| One component, scattered 64 × 64 | 8.868 | 16.140 | 3 |
| Eight components, same matrix and selection | 23.688 | 23.017 | 3 |
| 32 components, same matrix and selection | 71.884 | 48.499 | 3 |
| Contiguous 256 × 128 inside one component | 17.024 | 22.867 | 3 |
| Contiguous 256 × 128 crossing a boundary | 18.486 | 22.667 | 1 |
| Interleaved reverse/duplicate positions, 64 × 64 | 65.105 | 22.918 | 1 |
| 512 scattered rows, all columns | 25.448 | 23.026 | 3 |
| Higher density, 512 scattered rows, all columns | 28.742 | 27.770 | 3 |
| All rows, 64 scattered columns | 58.464 | 23.984 | 1 |
| Gzip, scattered 64 × 64 | 33.386 | 27.049 | 1 |
| Reuse sparse workload, first read | 24.030 | 23.076 | 3 |
| Reuse higher-density workload, first read | 26.784 | 27.119 | 3 |

One component favors C++ by about 1.82×. At eight components the scattered
selection is within 3%; at 32 components C++ takes 1.48× Python's time. The
single reverse/duplicate and all-row controls take 2.84× and 2.44× Python's time.
These are specific performance gaps, rather than evidence of uniformly good
collection performance. Three observations do not establish statistical
equivalence or tail behavior.

The thirteenth workload uses positive-stride slices selecting 64 × 64 positions.
C++ returned the correct result. Native SnapATAC2 slices returned **8065 × 63**,
failing the independent oracle. Its invalid output has no timing ratio. This is
a finding about this pinned reader's slice route; it does not establish that
Python integer-index arrays cannot express the same selection. The original
failure and traceback remain in the receipt.

## Reusing an open collection

Each of three processes performs one first read followed by four repetitions.
The table takes the median within each process, then the median of those three
medians. All repeated outputs are verified outside operation clocks.

| Repeated API operation | AnnData++ | SnapATAC2 |
|---|---:|---:|
| 64 scattered rows, all columns, 16 stored values/row | 4.318 ms | 2.129 ms |
| 256 scattered rows, all columns, 128 stored values/row | 5.861 ms | 3.906 ms |

C++ reuses its prepared plan and caller-owned buffers; Python repeats native
indexing on the same open collection and allocates a new result. These compare
available API lifecycles, not identical kernels. C++ still takes **2.03×** and
**1.50×** Python's time. Setup amortization alone does not resolve the gap. This
measurement does not establish allocation-free execution.

## Where further investigation would help

The C++ phase medians for 1/8/32 components are respectively 5.75/13.46/37.46 ms
for opening, 1.79/5.51/17.95 ms for preparation, and 0.87/4.32/16.03 ms for
transfer. All three phases grow with component count. C++ opening is already
faster than Python's 14.12/20.04/40.76 ms on these cases. The comparative deficit
is after opening: at 32 components, C++ preparation and transfer take 17.95 and
16.03 ms, while Python's entire indexed operation takes 7.74 ms. Those phases
have different contracts, so this is a location of cost rather than an
equivalent-kernel speed ratio. The reverse/duplicate
control spends 25.57 ms preparing and 27.15 ms transferring, in addition to
opening. No single preparation-only correction explains all these gaps.

Separate diagnostics for the eight-component 64-row scattered read observe
128 H5Dread calls during preparation and 192 during transfer, with 2,921 and
2,552 intercepted C++ allocation requests. The 512-row higher-density read
observes 1,024 and 1,536 H5Dread calls. These support investigating acquisition,
segment reuse and sparse row traversal using the existing storage fast paths.
They do not prove a particular fix or measure physical device traffic. The
diagnostics cover first reads, not allocation behavior of the repeated loops.

## Workload and evidence boundaries

Five collections have shape 8192 × 2048: the same 16-values-per-row matrix split
into 1, 8 and 32 components; an eight-component 128-values-per-row matrix; and an
eight-component gzip-1 variant with 4,096-value data/index chunks and
1,024-offset chunks. Native all/range selectors are used where applicable on
both sides; irregular and duplicate selections use integer arrays. Every
successful result passes an independent cellwise formula and matching canonical
CSR digest. Canonical sorting and width conversion occur outside the clock;
native sparse entry order may differ. C++ retains int64 output offsets while
Python returns int32; both return float32 values and int32 column indices.

Clocks exclude imports, case parsing, verification, hashing and close. C++ first
reads include structural preparation, exact requirements and buffer allocation;
Python includes native selection and result allocation. SnapATAC2 executes
native Rust code, so this is a package/API comparison rather than a language
comparison. Package versions, the native SnapATAC2 binary and consumed C++
library identities are recorded without upgrading the reference environment.

The three windows ran once on CPU 2 of an Intel Xeon E5-2698 v4 host, with six compute-thread limits of one and
normal OS/backend caches: **24.749, 16.234 and 17.052 seconds**, totaling
**58.034 seconds**. Startup and oracle work count toward each 30-second window.
There were **66 processes: 65 passed and one returned incorrect Python output**.
The screen checkpoint preserves that failure; the other windows completed.
A separate post-campaign [analyzer](../tools/format-family-screen/collection-matrix/analyze.py)
accepts only the exact recorded failure and leaves the frozen original sources
unchanged. The original strict summarizer continues to reject that failed
process; its failure was not converted into a passing benchmark. All 128 distinct frozen inputs
were reverified. New fixture files, inventories and preserved partial setup
total **25,829,811 bytes**, below the approved 128 MiB limit.

Fresh processes are not cold-cache tests. A pre-run host snapshot found no
other benchmark/build process, but the host was not exclusively reserved. CPU 2
differs from the earlier screen's CPU 0, so old absolute timings are not a
controlled before/after comparison. Counters exclude unobserved allocators and
physical I/O. No claim follows about mmap throughput, dense/CSC collections,
large-memory workloads, every family format or universal absence of regression.
Existing mmap/pointer qualification was not rerun because library code did not
change. Raw receipts, plans, frozen identities, setup failure and checkpoints
remain under `build/collection-matrix-2026-11-17/`.

An independent read-only check reproduced every reported median and reuse
aggregation, checked the timer/oracle contracts, and verified all frozen inputs,
plans, process counts and successful output digests. It launched no additional
read workloads. Including campaign setup, the three window wall times were
25.320, 16.813 and 17.654 seconds, each still within its approved 30-second cap.
