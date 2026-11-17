# Collection execution overhead: investigation and fix plan

The first fix should make collection preparation retain useful execution
resources and stop describing the same stored matrix repeatedly. Numerical
batching is a separate step. Simply routing collections through the existing
bulk transfer entry point would lose validation and width-conversion behavior.

This is a plan, not an implemented repair. Production code, validation policy,
mmap and direct-pointer paths are unchanged. The source baseline is `7f8d72a`;
its library implementation is `32852b6`. The earlier [Python comparison](collection-matrix-performance.md)
locates the comparative deficit after opening. The new
[counter record](../benchmarks/results/collection-overhead-diagnostic.json)
separates preparation, explicit preflight and repeated execution.

## What the bounded diagnostic established

One instrumented window took **0.687 seconds including runner setup**, below its
30-second cap. Six processes each checked a first read and three repeats:
**24 correct outputs**, matching the independent formula and previous digests.
All 94 frozen input identities matched. No new stores or Python workloads were
created. These are diagnostic counts, not instrumented speed comparisons.
Independent artifact-only verification confirmed the hashes, schedule, output
digests and all reported counter relationships without rerunning a workload.

| Same 64 selected rows | Prepare: object opens / C++ new | Explicit preflight: object opens / C++ new | Each inclusive read: object opens / C++ new |
|---|---:|---:|---:|
| 1 component/run | 57 / 706 | 8 / 75 | 22 / 319 |
| 8 components/runs | 183 / 2,921 | 64 / 600 | 176 / 2,552 |
| 32 components/runs | 615 / 10,507 | 256 / 2,400 | 704 / 10,208 |
| Interleaved duplicates: 56 runs over 4 components | 1,047 / 12,444 | 448 / 4,200 | 1,232 / 12,320 |

Every first and repeated read paid the same counts. Preparation made 128 numeric
H5Dread calls; transfer made 192 in all four cases. Thus metadata/acquisition
work scales with ordered component runs, while this scanner's numeric calls
scale with selected rows. The separate explicit preflight is not subtracted
from a read: each `read_into()` includes its own necessary preflight.

A 256-row contiguous control still made 512 sizing and 768 transfer H5Dread
calls, despite touching only one component. Resource reuse alone will not
remove that per-row pattern.

The source explains the counts:

- `dataset.cpp` initializes layout/dtypes using four public getters, each of
  which describes the matrix again. This contributes a fixed acquisition cost.
- `PreparedReadImpl` retains a node and requirements, but no execution handles,
  selector lookup or staging workspace.
- Collection `read_into()` preflights all pieces; each component's subsequent
  `PreparedRead::read_into()` describes it again. Each read accounts for two
  eight-open descriptions plus six path-component opens for numeric objects.
- `scan_sparse()` constructs its lookup, vectors and numeric transfer owners
  afresh. Handles survive one scan, rather than the prepared execution lifetime.

The relevant implementations are `src/dataset.cpp` (`prepare_x`,
`each_prepared`, `sparse_plan`) and `src/backed_array.cpp` (`describe`,
`scan_sparse`, `PreparedRead`, `TransferContextImpl`). `H5Dopen2` counted zero
because these objects are acquired through `H5Oopen`; zero did not mean no
dataset acquisition.

## Proposed first implementation

1. **Fuse checked metadata acquisition.** Obtain the first component's layout,
   value dtype and independent sparse widths through one checked X binding.
   Preserve the first-component baseline even when it is unselected, and the
   current empty-selection behavior. Do not call whole-array requirements or
   scan an unselected matrix merely to obtain metadata.

2. **Retain worker execution resources using existing owners.** Keep shared,
   copyable `PreparedRead` immutable. Put mutable numeric handles, lookup and
   reusable staging in the move-only collection execution owner, reusing
   `TransferContext` and the existing private transfer machinery through small
   compiled internal bridges. Use one physical binding per participating
   component, shared across its ordered pieces. Bind the complete physical X,
   keyed by retained session/root/path and component identity, rather than the
   first selected piece or path text alone. Pass each piece's immutable axes and
   requirements into the checked scanner without copying physical metadata
   maps. Piece selections and output order remain explicit. Backend-owned
   descriptors and HDF5 types stay private.

   Preparation should derive pieces and their checked scans from the same
   component binding. Component 0's baseline signature needs metadata only;
   acquire numerical handles lazily if it participates. Reuse minor-axis lookup
   only when its selection agrees; CSC pieces can have different minor axes.

   Do not call public `prepare_transfer()` separately for every piece: that
   forks/reopens the backend. Internal serialized collection execution can
   borrow the retained session backend. Public per-worker contexts keep their
   existing private forks and concurrency contracts. No global cache, shared
   mutable `PreparedRead`, implicit locking or thread pool is needed.

3. **Preflight once, execute the checked binding.** Refresh/check physical
   metadata once for each participating component in an operation, then check
   every piece's axes, widths and destination against that binding. Complete
   all-piece destination preflight before the first output write. Execute
   through an internal preflighted helper so component reads do not repeat the
   same acquisition. Keep numerical sparse corruption checks in the existing
   scanner during sizing and transfer.

   Account descriptor, lookup, selector and staging capacities in the existing
   planning/scratch budgets. Retaining execution state must not multiply the
   budget by giving every piece a maximum-sized workspace. Preserve the bounded
   streamed fallback when resources do not fit; it may repeat acquisition.
   Repeated in-capacity scan staging should reuse capacity. This is not a promise
   that an entire HDF5 operation performs no allocations.

The public collection API and `check_axis_names` policy should remain unchanged.
Any internal bridge declarations should use existing exported owner/buffer
types; ordinary code and new private fields stay out of module interfaces.
Recheck the source baseline before implementation to accommodate separate
small-collection work without duplicating its changes.

## Subsequent work depends on the first screen

Reuse the existing bulk physical-read helpers beneath the checked scanner only
if remaining counters and timings justify it. Coalesce genuinely adjacent
selected rows within bounded staging, retaining the scanner's filtering and
width conversion for partial-minor selections. Do not read intervening rows to
turn a scattered selection into a rectangle. The main scattered-row workloads
cannot benefit from consecutive-run copying alone.

The current bulk entry point is not a drop-in replacement: it uses endpoint-only
sizing, requires native output widths and conditionally validates indices under
`Validation::full`. The scanner checks interior offsets, supports widened
destinations and rejects invalid source indices even when filtered out and
under `Validation::none`. Reuse physical read helpers while retaining those
stronger checks, source-entry order, duplicate multiplicity and explicit zeros.

There is also an unresolved selective-I/O issue. The 8-component scattered read
returned 1,053,248 intercepted libc read bytes for approximately 9,216 bytes of
logical sparse-array requests; higher density returned 8,318,200 against about
266,240 logically requested bytes. Those totals include metadata/read-ahead and
are not physical disk traffic, peak memory or proof of full materialization.
HDF5's contiguous-data sieve is a plausible contributor; it is distinct from
the bounded chunk cache already configured by this backend.

First check how retained handles change these counts. If amplification remains,
use one bounded path/offset-attributed probe and controls for contiguous,
scattered and chunked/gzip reads before considering a private backend policy
change. Do not globally disable the sieve or add a public tuning knob on the
present evidence.

## Qualification and stop conditions

- Keep preflight errors from touching output, including a failure in the last
  participating component. Preserve close/replacement invalidation, rooted
  alignment checks, empty selections, labels and opt-in name validation.
- Retain malformed interior/boundary offset and filtered-out invalid-index
  rejection, source duplicates/order/zeros, mixed widths, overflow, reverse
  selections, dense signed strides, CSC behavior and tight-budget fallback.
- Run Debug, affected ASan/UBSan, HDF5/Zarr affected tests, existing worker-context
  checks and the real mmap/physical-pointer audit. Keep mapping implementation,
  eligibility and explicit failure behavior unchanged. Check installed imports
  through both facades and public BMI/consumer rebuild isolation.
- Freeze before/after libraries and run at most **two 30-second comparison
  windows**, using the existing fixtures and native SnapATAC2 routes. Prioritize
  32 components, reverse/duplicates, both reuse workloads, and one-component /
  contiguous / gzip controls. Primary rows get three observations; auxiliary
  controls may get one. Separate counters from uninstrumented timings and
  compare identical requested outputs. Keep C++ before/after validation settings
  and output widths identical; report native Python widths and validation/API
  differences explicitly. Any further attribution probe must fit the remaining
  diagnostic budget; do not silently extend the campaign.
- Acceptance requires removing the measured duplicate acquisitions and repeated
  numeric-handle/workspace construction on retained paths, improving targeted
  end-to-end/reuse measurements, and checking both returned bytes and allocation
  counts. Flag a regression exceeding both 10% and 0.1 ms; small samples are a
  screen, not statistical proof. Do not declare Python parity without a measured
  equivalent-output comparison.

Stop after the first resource-reuse fix and its screen if it resolves the gaps.
Only pursue batching or HDF5-policy work when remaining evidence identifies
it as material. No speedup is yet measured or promised. Raw evidence, frozen
probe sources, setup failures and checkpoints are retained under
`build/collection-overhead-2026-11-17/`.
