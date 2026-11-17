# Collection metadata reuse screen

This harness stages the existing qualified collection oracle and small wrappers, builds two installed-prefix consumers, freezes inputs, and summarizes one bounded serial comparison. It does not generate fixtures. The reference root must contain the previously qualified fixtures, identities, baseline prefix, Python environment, and counter libraries at the paths checked by `freeze.py`. Supply paths explicitly; the experiment root need not be the original worktree.

From the implementation checkout, choose `EVIDENCE`, `REFERENCE_ROOT`, `BASELINE_PREFIX`, `CANDIDATE_PREFIX`, and Arrow/Parquet dependency paths. Stage source with:

```
python3 tools/format-family-screen/metadata-reuse/setup.py --repo "$PWD" --source-dir "$EVIDENCE/source"
```

For each of baseline and candidate configure `cmake -S "$EVIDENCE/source" -B "$EVIDENCE/<version>-consumer" -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX"` with the appropriate `ArrowParquet_INCLUDE_DIR`, `ArrowParquet_ARROW_LIBRARY`, and `ArrowParquet_PARQUET_LIBRARY`; then `cmake --build "$EVIDENCE/<version>-consumer" --parallel 2`. Use the same toolchain and dependency settings for both. `setup.py` extracts the unchanged helper prefix before `main` from the tracked qualified `collection-fix/cpp_probe.cpp`; existing differing staged files cause an error.

Once both consumers and candidate prefix are ready, freeze without running fixtures:

```
python3 tools/format-family-screen/metadata-reuse/freeze.py --repo "$PWD" --reference-root "$REFERENCE_ROOT" --evidence "$EVIDENCE" --candidate-prefix "$CANDIDATE_PREFIX"
```

After a quiet-window grant run once using the reference root's `tools/format-family-screen/window.py --plan "$EVIDENCE/plan.json" --output "$EVIDENCE/receipt.jsonl" --case-timeout 5`; preserve failures and do not retry within the same window. Summarize with `python3 tools/format-family-screen/metadata-reuse/summarize.py --evidence "$EVIDENCE"`.

The plan includes three fresh-process baseline, candidate, and Python observations for each of two fixture geometries and separate C++ diagnostic processes. Selector creation is outside both small-case clocks. The 10k control uses the qualified original C++ clock boundary, identical in both builds. Python and C++ have different libraries and validation; output offset widths differ in the 10k fixture. Interposer counters describe intercepted activity, not physical I/O. Three observations and normal caches limit generalization; phase medians need not add to end-to-end medians.
