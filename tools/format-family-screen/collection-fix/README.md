# Collection preparation comparison

This installed-consumer probe reuses the release11 collection cases and their
cellwise CSR oracle. Compile `cpp_probe.cpp` with the adjacent CMake project once
against each frozen Release installation, using identical toolchain and build
options. The copied probe adds scoped metadata and HDF5-read counts; use the
same bytes for both consumers. The Python comparator is release11's native
`X[rows, cols]` path and does not perform C++'s selected-axis checks.

The `freeze.py` helper verifies the original fixtures against the release11
inventory and binds the baseline, candidate, consumers, interposers, source,
interpreter and fixture paths into a single plan. Its evidence directory must
contain `source/` with the compiled probe sources, `baseline-consumer/`,
`libmetadata.so`, and the frozen candidate consumer at the supplied path. It
uses the existing collection-preparation I/O and allocation interposers; build
`metadata.c` as a shared library with `clang-18 -shared -fPIC -O2 -ldl`.

```sh
python3 tools/format-family-screen/collection-fix/freeze.py \
  --evidence EVIDENCE --candidate-prefix CANDIDATE_PREFIX \
  --candidate-exe CANDIDATE_CONSUMER/release11-cpp
python3 tools/format-family-screen/window.py --plan EVIDENCE/plan.json \
  --output EVIDENCE/receipt.jsonl --samples 1 --case-timeout 5
python3 tools/format-family-screen/collection-fix/summarize.py --evidence EVIDENCE
```

The plan has one aggregate window capped at 30 seconds, three fresh-process
observations per build and geometry, and separate interposed C++ diagnostics.
Coordinate the window with other work. Python must not receive the HDF5
interposers. `metadata_slots` contain H5Oopen, H5Dget_type, H5Dget_space,
H5Dget_create_plist, H5Aread, H5Gopen2, and H5Dread with string memory type.
Counters reflect intercepted calls, not physical I/O or total allocations;
diagnostic durations are not timing evidence. The summary checks frozen input
hashes, every process result and cross-language canonical FNV digest. Full
validation and the native sparse dtypes remain part of the C++ contract.

`check_validation.py FIXTURE_DIRECTORY NEW_DESTINATION` is an untimed semantic
check, separate from the campaign. It copies the three small collection files,
changes a selected manifest observation name or component variable name using
the stored `_index` attribute, and runs native Python X selection plus the full
payload oracle. The pinned SnapATAC2 reader accepted both mismatches; C++ retains
its mismatch rejection tests. Existing files and measurement fixtures are not
modified, and this check supplies no performance sample.
