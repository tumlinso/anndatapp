# Dense FP16 qualification

This Linux campaign uses deterministic 4096 × 1024 matrices whose values are exactly
representable in binary16. The FP16 and FP32 fixtures have identical shapes, values,
and physical read geometry. It covers contiguous and gzip-chunked H5AD, ordinary
Zarr v2 and v3, and sharded Zarr v3. Every read is checked against the formula. An
independent Python AnnData read checks the output of each write case before timing
and after the final native diagnostics.

The native cases measure caller-buffer dense copy, eager read and write, and
prepared caller-buffer slices that cross chunk boundaries. The sharded update
case writes four such rectangles to separate reusable fixture copies in each arm.
Timed updates alternate the signs of the selected entries while leaving the
surrounding data untouched. The public eager Zarr writer emits ordinary chunks.
Eager writes use the same native writer and compression settings across dtypes.
Automatic output chunk geometry can depend on item width. The result records
actual output geometry and encoded store size for those cases.

Python controls use AnnData for eager reads and h5py or Zarr directly for
prepared transfer equivalents. HDF5 uses `read_direct` and Zarr uses
`get_basic_selection(..., out=...)` or `set_basic_selection`. Import time is
outside both clocks. Setup covers buffer preparation and opening the backed
store. The eager operation includes file open/close and output allocation.
Python and native acquisitions use their ordinary public contracts and may
perform different metadata work. There is no eager Python write comparison
across differing output chunk and compression policies.

Each case uses five alternating uninstrumented sample pairs on the same CPU.
Native diagnostic samples reuse the standard release allocation and I/O observers.
They report intercepted calls and bytes, and prepared transfers also report
logical backend calls and retained scratch. The C++ allocation observer does
not account for arbitrary C or codec heap allocations. No timing or OS cache
eviction is added to the diagnostic counts. The runner enforces a 30-second
subprocess budget per measured case and checkpoints raw samples, source/binary
hashes, fixture identity, and library versions.

## Reproduce

Run from a frozen source snapshot with the pinned reference environment. Obtain
a quiet timing window on the selected Linux CPU. The optional repository
restricted-runner wakeup is supported when generating fixtures only.
Measurements require normal Python asyncio wakeups.

```sh
bench_dir=/tmp/anndatapp-fp16-qualification
bench_python=/path/to/pinned/reference/bin/python3
bench_cpu=2
cmake -S benchmarks/fp16 -B "$bench_dir/bin" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DANNDATAPP_SOURCE="$PWD"
cmake --build "$bench_dir/bin" --parallel 2
"$bench_python" benchmarks/fp16/run.py fixtures "$bench_dir/fixtures"
"$bench_python" benchmarks/fp16/run.py run --fixtures "$bench_dir/fixtures" \
  --binary "$bench_dir/bin/fp16_benchmark" --output "$bench_dir/results" --cpu "$bench_cpu"
```
