# Format-family repair checkpoint

Implementation and qualification are complete for the approved repair scope. Source commit: `bad691a8fe4a83452fbe882720feee8583d79733`, based on `3a35c6f051cbd9125a75277d4b32c0a8616b5940`. Branch: `codex/format-family-repairs`.

The [repair validation report](format-family-repair-validation.md) and [machine-readable evidence](../benchmarks/results/format-family-repair.json) are the current handoff. They cover the five correctness repairs, the additional reviewed shape-validation gap, lazy maps/collection validation/tree decoding, preserved mmap pointers, nine passing build/test configurations and the bounded sparse screen. Historical implementation and review reports remain preserved.

Persistent evidence has been copied and hash-checked under `/home/tumlinson/anndatapp/build/format-family-repair-qualification-2026-11-15` before publication. The source-input manifest, frozen baseline hash manifest, test XML/logs, mmap reproduction, rebuild receipts, raw screen data and source recovery bundle are required recovery inputs. Generated reference environments and stores remain ignored. Nothing required for recovery relies on `/tmp`.

The screen completed in 25.297 seconds with one sample per case. It flags collection subset-read overhead for discussion; no deeper campaign is authorized by this checkpoint. Baseline collection processes timed out at the three-second process limit and provide no measured speedup ratio. See the report for exact timing boundaries and unmeasured quantities.

Publication is authorized: verify clean primary main, merge the repair branch and push origin/main. The persistent evidence directory's delivery receipt and live Git state record whether publication has completed. The desktop app previously rejected managed worktree archiving because it is the task's primary checkout; do not manually bypass that restriction. Recheck actual Git and artifact state on resume before repeating publication. Do not reset, clean or overwrite unrelated work.
