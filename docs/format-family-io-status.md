# Format-family I/O checkpoint

The approved work uses pre-extension commit `e8c51ce7c3d8c2ded9d1b5f43c1e4d035ea13629` as the capability/performance reference and starts from `bbe693ce53a729cc8bc475cf56ee55bf1a33a666` on `codex/format-family-io`. Every later implementation choice is replaceable; published history remains preserved.

Performance of the shipping I/O path leads. Preserve direct buffers/mmap, explicit ownership, selections and sparse widths/order, caller execution, bounded scratch, adaptive/manual streams, all four formats, their compatibility matrix and both facade imports. Apply the written compact style with AnnData++'s ownership/I/O exception. No second array model or generic storage/execution framework.

## Approved implementation

- Add move-only `PreparedAnnDataSetRead` via `AnnDataSetView::prepare_x()`, with cached exact requirements, complete destination preflight and caller-buffer transfers. Retain existing component PreparedReads within the planning budget; regenerate bounded pieces when retention cannot fit. Convenience methods prepare ephemerally; materialization prepares once. Collection close/structural edits invalidate retained snapshots. Component sessions remain read-only; independent external mutations require caller synchronization.
- Batch selected-axis comparison and reuse reference batches across components. Add private bounded gathered one-dimensional string transfers, HDF5 point reads with existing string-budget/reclamation machinery, rooted forwarding and a bounded generic fallback. Preserve order, duplicates, UTF-8 diagnostics and oversized-string errors.
- Resolve sparse adjustment integer types outside element loops. Expose SpatialData I/O control flow and share concrete metadata construction where useful. Document native MuData slicing's existing allocation/projection costs without changing its snapshot/thread-use semantics.

## Evidence and acceptance

Use existing benchmark infrastructure and pinned reference environments. Compare pre-extension/current/candidate ordinary AnnData; compare current/candidate extensions and Python MuData, TreeData, SnapATAC2 and SpatialData, plus Python AnnData controls. Separate open/preparation, prepared transfer and equivalent end-to-end outputs. Record matching source/toolchain/fixtures and instrumentation limits.

Cap each core/format screening window at 30 seconds, initially one sample and at most three for flags within the same limit. Additional generated fixtures are capped at 256 MiB. Larger/deeper campaigns require discussion. Preserve native/interop/optional-build/sanitizer/installed-consumer checks, mmap lifetime checks, private-rebuild isolation and independent style review. No unresolved reproducible material regression may be silently accepted.

## Current state and recovery

Implementation is in progress, not qualified. Root owns integration, CMake, qualification, documentation and publication. Collection changes belong to `lazy_repair_design`; private gathered metadata to `io_metadata`; focused formatting/docs to `io_style`; screen preparation/interpretation to `selective_io_fast_path_audit` and its peers. Owners must coordinate shared headers and keep performance measurements serial in root-granted quiet windows.

Persistent artifacts are under `build/format-family-io`. `checkpoint.json` identifies the frozen starting library in `build/format-family-repair/candidate-prefix`; `pre-extension-source.tar` preserves the exact original source and its isolated build/install paths. Keep all existing repair evidence. Required recovery inputs do not rely on `/tmp`.

On restart, inspect live Git/source/agent state before repeating work. Preserve unrelated edits; never reset or clean. After validated commits, preserve raw receipts and a recovery bundle in the primary checkout, merge/push main under the existing authorization, and use app-managed cleanup only when permitted. The app previously refused removal of this task's active primary worktree.
