#!/usr/bin/env python3
"""Run exactly one release 1.1 comparison case in a fresh process."""
import argparse
import json
import os
from pathlib import Path
import sys
import time

START = time.perf_counter_ns()
sys.path.insert(0, str(Path(__file__).parent.parent))
from python_family import (Observer, ROWS, COLS, _assert_csr, _legacy_10k_golden,
                           _legacy_spread_positions, digest, package_versions)


def fnv1a64(payload):
    value = 14695981039346656037
    for byte in payload:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def axis_names(group, positions):
    name = group.attrs['_index']
    if isinstance(name, bytes):
        name = name.decode()
    return group[name].asstr()[positions]


def validate_axes(path, rows, cols, h5py, np):
    # Only selected names are read. Component sizes establish global offsets.
    with h5py.File(path, 'r') as manifest:
        table = manifest['uns/AnnDataSet']
        paths = table['file_path'].asstr()[:]
        keys = table['keys'].asstr()[:]
        if len(set(keys)) != len(keys) or any(not k for k in keys):
            raise ValueError('invalid manifest component keys')
        manifest_obs = axis_names(manifest['obs'], rows)
        manifest_var = axis_names(manifest['var'], cols)
        offset = 0
        participating = 0
        for component_path in paths:
            component_path = Path(component_path)
            if not component_path.is_absolute():
                component_path = path.parent / component_path
            with h5py.File(component_path, 'r') as component:
                obs = component['obs']
                index = obs.attrs['_index']
                n = len(obs[index])
                mask = (rows >= offset) & (rows < offset + n)
                if np.any(mask):
                    participating += 1
                    if not np.array_equal(axis_names(obs, rows[mask] - offset), manifest_obs[mask]):
                        raise ValueError('selected observation names differ')
                    if not np.array_equal(axis_names(component['var'], cols), manifest_var):
                        raise ValueError('selected variable names differ')
                offset += n
        if offset != len(manifest['obs'][manifest['obs'].attrs['_index']]):
            raise ValueError('component observation counts differ')
        return participating


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('case', choices=[f'raster-{kind}{suffix}' for kind in ('decimated', 'aligned', 'multichunk') for suffix in ('', '-raw')] + [f'dataset-{kind}{suffix}' for kind in ('small', '10k') for suffix in ('', '-validated')])
    parser.add_argument('fixture', type=Path)
    parser.add_argument('--diagnostic', action='store_true')
    args = parser.parse_args()
    if os.environ.get('ANNDATAPP_INTEROP_ZARR_WAKEUP', '0') != '0':
        raise RuntimeError('probe requires normal Zarr event loop (wakeup=0)')
    import numpy as np
    import dask
    import zarr
    dask.config.set(scheduler='synchronous')
    zarr.config.set({'async.concurrency': 1, 'threading.max_workers': 1})
    if args.case.startswith('raster-'):
        if not args.case.endswith('-raw'):
            import spatialdata as sd
    else:
        import snapatac2 as snap
        import h5py
    import_ns = time.perf_counter_ns() - START
    path = args.fixture.absolute()
    observer = Observer(args.diagnostic)
    phases = {}
    first_ns = last_ns = None
    def begin():
        nonlocal first_ns
        observer.begin()
        now = time.perf_counter_ns()
        if first_ns is None:
            first_ns = now
        return now
    def end(name, start):
        nonlocal last_ns
        now = time.perf_counter_ns()
        last_ns = now
        phase = observer.end()
        if phase is not None:
            phase.pop('h5dread_calls', None)
            phase.pop('h5dwrite_calls', None)
            phase['scope'] = name
            phase['io_scope'] = ('intercepted libc calls only; excludes HDF5 interposition, '
                                 'direct syscalls, mmap page-ins, hidden calls and physical I/O')
        phases[name] = phase
        return now - start

    if args.case.startswith('raster-'):
        raw = args.case.endswith('-raw')
        kind = args.case.removeprefix('raster-').removesuffix('-raw')
        ys, xs, width = {'decimated': (slice(16, 80, 2), slice(32, 96, 2), 256),
                         'aligned': (slice(16, 48), slice(32, 64), 256),
                         'multichunk': (slice(96, 224, 4), slice(96, 224, 4), 512)}[kind]
        start = begin()
        if raw:
            # Fixture admission resolves level 0 to s0 for both languages.
            # Open the target directly rather than add format discovery only
            # to the Python storage control.
            image = zarr.open_array(str(path/'images/image/s0'), mode='r')
        else:
            data = sd.read_zarr(path, selection=('images',))
            image = data.images['image']
            if hasattr(image, 'children'):
                level = 'scale0' if 'scale0' in image.children else next(iter(image.children))
                image = image[level]['image']
        open_ns = end('open', start)
        start = begin()
        if raw:
            selected = image.oindex[:, ys, xs]
        else:
            selected = image.isel(c=slice(None), y=ys, x=xs).data
            if hasattr(selected, 'compute'):
                selected = selected.compute(scheduler='synchronous')
        selected = np.ascontiguousarray(selected)
        operation_ns = end('operation', start)
        expected = np.array([[(width*y+x) % 65536 for x in range(*xs.indices(width))]
                             for y in range(*ys.indices(width))], dtype=np.uint16)[None, :, :]
        if selected.shape != (1, 32, 32) or selected.dtype != np.uint16 or not np.array_equal(selected, expected):
            raise ValueError('raster output does not match independent formula')
        output = {'shape': list(selected.shape), 'dtype': str(selected.dtype),
                  'contiguous': bool(selected.flags.c_contiguous), 'sha256': digest(selected.tobytes()),
                  'output_fnv1a64': fnv1a64(selected.astype('<u2', copy=False).tobytes(order='C'))}
        validation_ns = 0
        boundary = 'direct Zarr array at agreed level-0 s0 path' if raw else 'SpatialData image isel before synchronous compute'
    else:
        tenk = args.case.startswith('dataset-10k')
        rows = np.asarray(_legacy_spread_positions(10000) if tenk else ROWS, dtype=np.int64)
        cols = np.asarray(_legacy_spread_positions(20000) if tenk else COLS, dtype=np.int64)
        start = begin()
        data = snap.read_dataset(path, mode='r')
        open_ns = end('open', start)
        validation_ns = 0
        participating = None
        if args.case.endswith('-validated'):
            start = begin()
            participating = validate_axes(path, rows, cols, h5py, np)
            validation_ns = end('validation', start)
        start = begin()
        selected = data.X[rows, cols]
        operation_ns = end('operation', start)
        if selected.shape != (64, 64) or selected.data.dtype != np.float32 or selected.format != 'csr':
            raise ValueError('expected native float32 CSR output with shape (64, 64)')
        native = {'data': str(selected.data.dtype), 'indices': str(selected.indices.dtype),
                  'indptr': str(selected.indptr.dtype)}
        result = _assert_csr(selected, {'np': np}, _legacy_10k_golden(rows.tolist(), cols.tolist()) if tenk else None)
        canonical = (np.asarray(selected.data, dtype='<f4').tobytes(order='C') +
                     np.asarray(selected.indices, dtype='<u4').tobytes(order='C') +
                     np.asarray(selected.indptr, dtype='<u8').tobytes(order='C'))
        output = result | {'native_dtypes': native, 'participating_components': participating,
                           'output_fnv1a64': fnv1a64(canonical)}
        data.close()
        boundary = 'SnapATAC2 simultaneous X[rows, cols]' + (' with selected axis-name validation' if args.case.endswith('-validated') else '')
    receipt = {'case': args.case, 'fixture': str(path), 'open_ns': open_ns,
               'validation_ns': validation_ns, 'operation_ns': operation_ns,
               'end_to_end_ns': last_ns - first_ns,
               'end_to_end_scope': 'first open timestamp through final materialized output; includes phase boundaries',
               'python_imports_ns': import_ns, 'golden': 'pass', 'output': output,
               'api_boundary': boundary, 'measurement_mode': 'instrumented_diagnostic' if args.diagnostic else 'uninstrumented_timing',
               'diagnostic_phases': phases if args.diagnostic else None,
               'settings': {'dask_scheduler': 'synchronous', 'zarr_async_concurrency': 1,
                            'zarr_threading_max_workers': 1, 'zarr_wakeup': 0},
               'versions': package_versions()}
    print(json.dumps(receipt, sort_keys=True, separators=(',', ':')))

if __name__ == '__main__':
    main()
