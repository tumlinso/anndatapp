# Format reference environments

The locked files record the resolved packages used for independent interoperability tests; they are test-only dependencies and are not linked into AnnData++. `basic` is the current MuData/TreeData environment; `full` adds SnapATAC2, SpatialData, Arrow/Parquet and the HDF5 filter plugin. The legacy environments produce the older TreeData and MuData Zarr fixtures.

The qualification host used Python 3.12.3 for `basic`, `legacy-tree` and `legacy-mudata`, and task-local CPython 3.12.14 for `full`. `macs3`, a SnapATAC2 dependency, is built from source and needs Python development headers. A task-local interpreter avoids installing headers system-wide. For example, from the repository root:

```sh
export UV_CACHE_DIR="$PWD/build/reference/uv-cache"
uv python install --install-dir "$PWD/build/reference/python" --no-bin 3.12.14
"$PWD/build/reference/python/cpython-3.12.14-linux-x86_64-gnu/bin/python3.12" -m venv --without-pip build/reference/full
uv pip sync --python "$PWD/build/reference/full/bin/python3" tests/reference/requirements-full.lock
```

The other lock files can be installed into separate Python 3.12 virtual environments with `uv pip sync --python <environment>/bin/python3 <lock-file>`. Keep environments separate: legacy Zarr writers require `zarr 2`, while the current environment uses `zarr 3`.

On the restricted Linux runner, set `ANNDATAPP_INTEROP_ZARR_WAKEUP=1` and install the test-only event-loop policy from `tests/python_storage.py` before importing Zarr or AnnData. Ordinary hosts and CI use their normal event loop.

The `pyarrow 21.0.0` wheel in `full` supplies private C++ headers in `lib/python3.12/site-packages/pyarrow/include` and the versioned `libarrow.so.2100` and `libparquet.so.2100` shared libraries. Installed native consumers need their own supported Arrow/Parquet installation; the reference environment is not part of the package.

SnapATAC2 can write HDF5 datasets with the Zstandard filter. For native reference tests of such data, set `HDF5_PLUGIN_PATH` to `build/reference/full/lib/python3.12/site-packages/hdf5plugin/plugins`. A standalone C probe linked with HDF5 1.10.10 successfully read and checked a Zstandard fixture independently written by h5py/HDF5 2.0.0 with this filter. This probe does not qualify the collection implementation.

Generate an independently reopened current SpatialData fixture into a **new** directory with:

```sh
build/reference/full/bin/python3 tests/reference/generate_spatial_current.py build/reference/fixture-current-wkb
build/reference/full/bin/python3 tests/reference/generate_spatial_current.py build/reference/fixture-current-geoarrow --geoarrow
```

Each directory contains `fixture.zarr`, an upstream readback summary and a per-file SHA-256 manifest. Neither command overwrites an existing output directory. The generator includes multiscale image, labels, typed/categorical points, spatial transforms, polygons with holes, multipolygons, circles and an AnnData table.

Generate the two legacy SpatialData fixtures with their separately locked environments:

```sh
build/reference/legacy-spatial-02/venv/bin/python3 tests/reference/generate_spatial_legacy.py build/reference/fixture-legacy-02
build/reference/legacy-spatial-03/venv/bin/python3 tests/reference/generate_spatial_legacy.py build/reference/fixture-legacy-03
```

SpatialData 0.3.0 and 0.5.0 both identify the store container as version `0.1`: their individual element versions differ. The legacy locks pin GeoPandas 1.0.1 and setuptools 80.9.0 to preserve compatibility with these writers.
