#!/usr/bin/env python3
"""Generate bounded, deterministic, uncompressed H5AD sparse collection inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import h5py
import numpy as np

LIMIT = 256 * 1024 * 1024
NVARS = 20_000
NNZ = 100
SIZES = (10_000, 100_000)

def attr(obj, kind, version):
    obj.attrs['encoding-type'] = kind
    obj.attrs['encoding-version'] = version

def strings(group, name, values):
    ds = group.create_dataset(name, data=np.asarray(list(values), dtype=object), dtype=h5py.string_dtype('utf-8'))
    attr(ds, 'string-array', '0.2.0')

def table(parent, name, index):
    group = parent.create_group(name)
    attr(group, 'dataframe', '0.2.0')
    group.attrs['_index'] = '_index'
    group.attrs['column-order'] = np.asarray([], dtype=h5py.string_dtype('utf-8'))
    strings(group, '_index', index)
    return group

def empty_maps(f):
    for name in ('obsm', 'varm', 'obsp', 'varp', 'layers', 'uns'):
        attr(f.create_group(name), 'dict', '0.1.0')

def file_base(path, start, rows):
    f = h5py.File(path, 'x')
    attr(f, 'anndata', '0.1.0')
    table(f, 'obs', (f'cell-{i:06d}' for i in range(start, start+rows)))
    table(f, 'var', (f'gene-{i:05d}' for i in range(NVARS)))
    empty_maps(f)
    return f

def component(path, start, rows):
    with file_base(path, start, rows) as f:
        x = f.create_group('X')
        attr(x, 'csr_matrix', '0.1.0')
        x.attrs['shape'] = np.array([rows, NVARS], dtype=np.int64)
        data = x.create_dataset('data', shape=(rows*NNZ,), dtype='<f4')
        indices = x.create_dataset('indices', shape=(rows*NNZ,), dtype='<i4')
        indptr = x.create_dataset('indptr', data=np.arange(rows+1, dtype=np.int64)*NNZ)
        for ds in (data, indices, indptr):
            attr(ds, 'array', '0.2.0')
        positions = np.arange(NNZ, dtype=np.int32)
        for first in range(0, rows, 1000):
            n = min(1000, rows-first)
            r = np.arange(start+first, start+first+n, dtype=np.int32)[:, None]
            indices[first*NNZ:(first+n)*NNZ] = (positions*199 + r%97).ravel()
            data[first*NNZ:(first+n)*NNZ] = (1 + (r+positions)%31).astype(np.float32).ravel()

def manifest(path, rows):
    with file_base(path, 0, rows) as f:
        t = table(f['uns'], 'AnnDataSet', (f'part-{i}' for i in range(8)))
        t.attrs['column-order'] = np.array(['keys', 'file_path'], dtype=h5py.string_dtype('utf-8'))
        strings(t, 'keys', (f'part-{i}' for i in range(8)))
        strings(t, 'file_path', (f'part-{i}.h5ad' for i in range(8)))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out', type=Path)
    a = parser.parse_args()
    # Bound includes payload, conservative HDF5 string heaps/metadata, all 20 files.
    upper = sum(2*n*NNZ*8 + 2*n*8 + 2*n*64 + 11*NVARS*64 + 20*1024*1024 for n in SIZES)
    if upper > LIMIT:
        raise SystemExit(f'predicted fixture upper bound {upper} exceeds {LIMIT}')
    a.out.mkdir(parents=True, exist_ok=False)
    for n in SIZES:
        d = a.out / str(n)
        d.mkdir()
        for i in range(8):
            start, end = n*i//8, n*(i+1)//8
            component(d / f'part-{i}.h5ad', start, end-start)
        component(d / 'monolithic.h5ad', 0, n)
        manifest(d / 'collection.h5ads', n)
    files = sorted(a.out.rglob('*.h5ad*'))
    size = sum(p.stat().st_size for p in files)
    if size > LIMIT:
        raise SystemExit(f'fixture size {size} exceeds {LIMIT}; preserve files for inspection')
    inventory = {str(p.relative_to(a.out)): {'bytes': p.stat().st_size,
                 'sha256': hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()} for p in files}
    (a.out / 'inventory.json').write_text(json.dumps({'bytes': size, 'limit': LIMIT, 'files': inventory}, indent=2)+'\n')
    print(f'Fixture files: {size} bytes, limit {LIMIT}')

if __name__ == '__main__':
    main()
