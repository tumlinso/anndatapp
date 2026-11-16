# Collection preparation probe

`instrument.py SOURCE DEST` copies a checkout, excludes its `.git`, `build`,
`.venv` and generated state, then adds phase hooks only to `DEST/source`. It
accepts frozen source snapshots as well as working checkouts. Keep the original
source immutable and use a new destination for each copy.

Configure each copy with the same Clang/HDF5 dependencies and options, then
build `collection-phase-probe`, `collection-io`, `collection-alloc` and
`collection-metadata`:

```sh
cmake -S DEST/source -B DEST/build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=DEPENDENCY_PREFIX \
  -DANNDATAPP_ENABLE_HDF5=ON -DANNDATAPP_ENABLE_ZARR=OFF \
  -DANNDATAPP_ENABLE_PARQUET=OFF -DANNDATAPP_BUILD_TESTS=OFF \
  -DANNDATAPP_BUILD_EXAMPLES=OFF -DANNDATAPP_BUILD_BENCHMARKS=OFF
cmake --build DEST/build --parallel 2 --target \
  collection-phase-probe collection-io collection-alloc collection-metadata
```

Run the same manifest serially with `collection-phase-probe MANIFEST timing`
and `control`. For one bounded diagnostic, use `diagnostic` and preload the
three libraries in this order: `libcollection-metadata.so`, `libcollection-io.so`,
`libcollection-alloc.so`. `control-diagnostic` runs the same interposers with
phase clocks disabled. `tools/format-family-screen/window.py` can execute a
frozen plan on one CPU with a 30-second window and records hashes, affinity,
thread limits, cache policy and each process receipt.

Phase `numeric_access` includes ordinary `read_numeric` calls on the baseline
and `prepare_numeric` plus `NumericTransfer::read` on the candidate. H5Dread
calls are counted in their own nested phase. The phase wrappers are applied by
the same script to both copies; the probe checks all 512 sparse values and
indices and 65 offsets against the formula oracle. Interposed runs are for call,
byte and allocation counts; use `timing`/`control` for elapsed time. Some phase
hooks alter temporary destruction timing slightly, so treat subphase
attribution as approximate.

The installed-prefix CPU sampler repeats only `prepare_x()` and
`requirements()` inside the perf control region, then checks the sparse output
outside it. Build it against the candidate's installed package:

```sh
cmake -S tools/format-family-screen/collection-preparation/sampler \
  -B /tmp/anndatapp-collection-sampler -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=INSTALLED_PREFIX
cmake --build /tmp/anndatapp-collection-sampler --parallel 2
```

With fresh `CTL` and `ACK` FIFOs, run:

```sh
perf record -o PROFILE.data -e task-clock:u -F 999 \
  --call-graph dwarf,16384 --control=fifo:CTL,ACK --delay=-1 -- \
  /tmp/anndatapp-collection-sampler/collection-allocation-cpu-sampler \
  MANIFEST CTL ACK 64
```

The consumer accepts the observed `ack\n` frame and `ack\n\0` frame. Profile
samples are warm sampled self-cost evidence, not allocator wall time or a
single-call latency estimate.
