# Durable format-family checkpoint

Implementation and final build qualification are complete on `codex/anndata-format-family`. This file and the source are committed as a recovery checkpoint before the remaining delivery steps. All task files and evidence needed to finish are on persistent storage under `/home/tumlinson`; temporary directories are not required to resume.

## Authorized outcome

Finish the approved four-format implementation, preserve the repository/package/namespace `anndatapp`, expose `import anndata;`, merge the validated branch into local `main`, then archive its managed worktree. Do not publish a release or change downstream projects. Preserve unrelated work.

## Locations and accepted evidence

- Managed checkout: `/home/tumlinson/.codex/worktrees/anndata-format-family/anndatapp`.
- Primary checkout: `/home/tumlinson/anndatapp`; original clean main was `e8c51ce7c3d8c2ded9d1b5f43c1e4d035ea13629`.
- Frozen implementation/test input digest: `180223eca95339848eb144bfa3bd312e8fe4f2196d4c6fdb44f1848e2f68dff8`; per-file hashes are in `build/qualification/source-freeze-final.json` and `benchmarks/results/format-family-qualification.json`.
- Final matrix receipts: `build/qualification/*-delivery-{build.log,tests.log,tests.xml,full-tests.log}`. Debug 33/33, Parquet/reference 49/49, core-only 20/20, HDF5-only 28/28, Zarr-only 26/26, shared 34/34, Release 34/34, ASan/UBSan 34/34, focused TSan 4/4. The full reference suite includes current and selected legacy formats.
- The final raw-array geometry repair is included and the entire matrix above passed afterwards. It is a private implementation fix, with raw prepared-context read/write and lifetime regressions in `tests/test_store.cpp`.
- Private edit probe: `build/qualification/private-rebuild-delivery/result.json`; 15 public BMIs and two consumer objects unchanged; restored binaries match.
- Independent generators and six environment locks are tracked under `tests/reference/`. Raw environments/fixtures live under `build/reference/` and may be regenerated from those locks; they are not source dependencies.
- Final reference Python: `build/reference/full/bin/python3`. Private Arrow prefix: `build/reference/current/lib/python3.12/site-packages/pyarrow`. Existing storage prefix: `/home/tumlinson/anndatapp/build/dependencies/usr`.

## Remaining delivery steps at this checkpoint

1. Finish the bounded cost probe against the repaired Release installed prefix at `build/release/installed-test-v3/prefix`. Its owner is the Codex subagent `/root/bounded_cost_probe`; if unavailable, use the tracked recipe in `tools/format-family-probe/README.md`. Files/results live in `build/qualification/cost-probe/`; no `/tmp` artifact is needed. Import and transform measurements already passed. Preserve their pre-private-repair source scope. The transfer observation must use the repaired library, retain raw arrays without adding encoding attributes, validate output/padding, and record its hashes and scratch statistics. Save compact results to `build/qualification/cost-probe/result.json`.
2. Refresh the tracked qualification JSON with the final independent fixture records and measured probe results. Update `docs/format-family-validation.md` with the bounded observations and their limits. Do not claim general performance improvements.
3. Preserve raw logs/receipts/manifests outside the managed checkout at `/home/tumlinson/anndatapp/build/format-family-qualification-2026-11-14` before archiving. Include full CTest logs, the input hash manifest, private-rebuild receipt, and probe raw logs. Generated store binaries and environments are reproducible and need not be copied. Verify the copied file hashes.
4. Commit the final evidence/docs. Check both checkouts for unrelated edits. In the primary checkout, merge `codex/anndata-format-family` into `main` without resetting or overwriting user work. A fast-forward is appropriate if primary main remains at the recorded base.
5. Use the Codex app's `list_artifacts` and `archive_worktree` tools to archive the managed checkout after the merge. Its attachment identity is `/home/tumlinson/.codex/worktrees/anndata-format-family/anndatapp`. Do not delete the primary checkout or manually remove Git worktree state. Verify main HEAD, clean status and worktree closure, then report completion.

If interrupted, inspect actual Git state and these persistent receipts before resuming. Git operations require approved access to shared Git metadata. There is no need to rerun the accepted matrix unless implementation or required tests change.
