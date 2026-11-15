#!/usr/bin/env python3
"""Create a small contiguous dense H5AD plus independent expected payload hash."""
import argparse, hashlib, json
from pathlib import Path
import anndata as ad
import h5py
import numpy as np
p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); a=p.parse_args()
a.directory.mkdir(parents=True,exist_ok=False)
rows,cols=256,256
x=(np.arange(rows*cols,dtype=np.float32).reshape(rows,cols)%np.float32(251)).astype('<f4')
path=a.directory/'dense-contiguous.h5ad'
ad.AnnData(X=x).write_h5ad(path,compression=None)
with h5py.File(path,'r') as f:
    dataset=f['X']
    if dataset.chunks is not None or dataset.compression is not None or dataset.id.get_offset() is None:
        raise RuntimeError('fixture X is not allocated contiguous uncompressed storage')
hash_payload=hashlib.sha256(x.tobytes(order='C')).hexdigest()
with path.open('rb') as f: hash_file=hashlib.file_digest(f,'sha256').hexdigest()
record={'file':path.name,'file_bytes':path.stat().st_size,'file_sha256':hash_file,
        'payload_bytes':x.nbytes,'payload_sha256':hash_payload,'shape':[rows,cols],
        'dtype':'float32 native little endian', 'formula':'(row*256+column)%251'}
(a.directory/'expected.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record))
