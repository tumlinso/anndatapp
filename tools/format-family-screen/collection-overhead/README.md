# Collection preparation and repeated-read diagnostic

This probe reads the existing frozen matrix fixtures with the previously
qualified `32852b6` Release installation. It adds no production code or
fixture. The existing C++ formula oracle, selectors, destination buffers and
metadata/I/O/allocation interposers are reused. `handles.c` adds independent
counts of user-visible HDF5 dataset opens/closes.

Each process opens a collection, prepares once, allocates once, explicitly
preflights, and executes a first and three repeated `read_into` operations with
the same plan and buffers. Every output is verified outside the counters.
Each read includes its own whole-collection preflight; those counts are not
subtracted from or added to the separate explicit preflight. The explicit
preflight before the first read may warm internal caches.

The six single-process cases are the same 64-row spread across 1/8/32
components, one-component contiguous rows, the interleaved reverse/duplicate
selection, and the 256-row higher-density/all-columns reuse case. The output
records H5Dopen2/H5Dclose, H5Oopen and other metadata calls, H5Dread, observed
libc I/O, and C++ `new` requests and bytes. These are interposed API counts,
not physical device I/O, total allocation, peak memory, or zero-allocation
proof. Durations from interposed runs are not performance evidence.

The diagnostic must be run serially on an otherwise quiet host within one
30-second window. `freeze.py` checks the old fixture inventory and archive
identity, snapshots the new sources, and freezes all probe, executable,
interface, interposer, fixture and case inputs. `campaign.py E` checkpoints
and fsyncs state before starting, enforces a hard 30-second process-group
limit, and never automatically retries a started or failed run. It retains
partial receipts and failures. `summarize.py E` verifies input identities,
counts, complete results and exact hashes against the previous independent
oracle; it does not launch workloads. Existing sample order and library policy
are unchanged. The single interposed observation per geometry gives mechanism
counts, not statistically reliable timings.
