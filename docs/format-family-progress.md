# Durable format-family delivery record

The implementation, final qualification matrix, and bounded measurements are committed. The source checkpoint is `7999fa67a1b62fec7ce2533e135a4bd28f20d1d2`; subsequent commits carry the final evidence and this delivery record. Source recovery and completion do not depend on a running agent, its connection, or temporary files.

The local merge/archive state and preserved-file hashes are recorded outside the managed checkout at `/home/tumlinson/anndatapp/build/format-family-qualification-2026-11-14/delivery-state.json`. Inspect that receipt and actual Git state if delivery is interrupted. The source and qualification are described in [the format guide](format-family.md), [validation report](format-family-validation.md), and [machine-readable record](../benchmarks/results/format-family-qualification.json).

## Persistent locations

- Primary checkout: `/home/tumlinson/anndatapp`. The reviewed baseline was clean main `e8c51ce7c3d8c2ded9d1b5f43c1e4d035ea13629`.
- Task branch: `codex/anndata-format-family`.
- Managed checkout, until archive: `/home/tumlinson/.codex/worktrees/anndata-format-family/anndatapp`.
- Preserved source bundles, full test output, input hashes, raw measurement logs, fixture manifests, and private-rebuild receipts: `/home/tumlinson/anndatapp/build/format-family-qualification-2026-11-14`.
- All necessary source, six reference locks, and small fixture/probe generators are tracked. Large generated stores and environments are reproducible artifacts, not source dependencies.

The accepted implementation/test input digest is `180223eca95339848eb144bfa3bd312e8fe4f2196d4c6fdb44f1848e2f68dff8`; its per-file hashes are in the tracked qualification JSON. That includes the final raw-array prepared-context repair. Debug passed 33/33, Parquet/reference 49/49, core-only 20/20, HDF5-only 28/28, Zarr-only 26/26, shared 34/34, Release 34/34, ASan/UBSan 34/34, and focused TSan 4/4. The final private-edit probe preserved 15 public BMIs and two consumer objects. The bounded raw-array transfer passed all five samples.

## Completion and interrupted-delivery recovery

The user authorized completing the four formats, merging the validated task branch into local main, then archiving the managed worktree. Preserve unrelated work; publication and downstream changes are outside this task.

1. Inspect the persistent delivery receipt, both Git checkouts, and branch ancestry. Do not reset, clean, or overwrite unrelated work. Existing accepted qualification need not be repeated unless implementation or required tests change.
2. Before archiving, preserve the raw logs, manifests and source bundle in the primary checkout's persistent evidence directory. Verify `preserved-files.json` hashes and `git bundle verify` the source bundle.
3. If main does not yet contain the task branch, merge it into main in the primary checkout. A fast-forward is appropriate when main still points to the reviewed baseline; otherwise preserve intervening history. Record the resulting main commit in `delivery-state.json` using atomic replacement.
4. If the worktree is still active, use the Codex app's `list_artifacts` and `archive_worktree` tools with its exact attachment identity. Archive only after main contains the completed work and evidence is preserved. Do not manually delete the primary checkout, authority data or shared Git metadata.
5. Verify main HEAD and clean status, confirm worktree closure, then atomically mark the persistent receipt complete. The receipt is the local lifecycle record; the tracked documents remain useful after the managed path disappears.

A source bundle is a local recovery copy, not publication. Recreate optional reference environments from `tests/reference/README.md` and its locks when fresh qualification is wanted. Neither a transient fixture nor an old build directory is required to recover the implementation.
