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

Implementation is complete through `c0fefced2793e0edbf96567cbb99bac361b7070a`. All 173 implementation/test/build inputs match digest `026db73847482d6c932c1a9eeab23db0d490a36ca240723e72008980f4ea2953`. Nine final configurations passed, including 54 full interoperability checks, plus the legacy MuData recheck, mapping and private-rebuild checks. Independent source review and focused rechecks resolved the bitmap, tiny-budget and overlapping-staging findings.

The [I/O qualification report](format-family-io-validation.md) and [compact evidence record](../benchmarks/results/format-family-io.json) are the current handoff. Raw bounded screens, rejected attempts, immutable baselines and prefix receipts are under `build/format-family-io`. No further measurements are pending: all requested uninstrumented outputs passed, while five interposed Python HDF diagnostics are explicitly unavailable. Remaining raster I/O amplification and collection setup cost require a separate discussion, not an automatic larger campaign.

Persistent recovery evidence is saved under `/home/tumlinson/anndatapp/build/format-family-io-qualification-2026-11-15`, including exact source archives, frozen prefixes, manifests, logs, raw measurements, small family fixtures, failed harness sources and a recovery bundle. The delivery receipt there and live Git state identify publication/cleanup status. Nothing required for recovery relies on `/tmp`.

On restart, inspect live Git/source/agent state before repeating work. Preserve unrelated edits; never reset or clean. After validated commits, preserve raw receipts and a recovery bundle in the primary checkout, merge/push main under the existing authorization, and use app-managed cleanup only when permitted. The app previously refused removal of this task's active primary worktree.
