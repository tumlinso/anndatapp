#!/usr/bin/env python3
"""Untimed semantic check of the native Python comparison's validation boundary."""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).parent.parent))
from python_family import ROWS, COLS, _assert_csr

p = argparse.ArgumentParser()
p.add_argument('fixtures', type=Path)
p.add_argument('destination', type=Path)
a = p.parse_args()
if a.destination.exists():
    raise FileExistsError(a.destination)
import h5py
import numpy as np
import snapatac2 as snap

a.destination.mkdir(parents=True)
results = []
for case, filename, axis in [('manifest-obs-name', 'dataset.h5ads', 'obs'),
                             ('component-var-name', 'dataset-part-0.h5ad', 'var')]:
    root = a.destination/case
    root.mkdir()
    for name in ('dataset.h5ads', 'dataset-part-0.h5ad', 'dataset-part-1.h5ad'):
        shutil.copy2(a.fixtures/name, root/name)
    with h5py.File(root/filename, 'r+') as f:
        group = f[axis]
        key = group.attrs['_index']
        group[key][0] = 'mismatched-selected-name'
    result = {'case': case, 'reader': 'SnapATAC2 native read_dataset + X[rows,cols]'}
    try:
        data = snap.read_dataset(root/'dataset.h5ads', mode='r')
        try:
            selected = data.X[np.asarray(ROWS, dtype=np.int64), np.asarray(COLS, dtype=np.int64)]
            if selected.shape != (64, 64) or selected.data.dtype != np.float32:
                raise ValueError('unexpected sparse shape/dtype')
            oracle = _assert_csr(selected, {'np': np})
            result.update(accepted_mismatched_selected_name=True, payload_golden=oracle['golden'])
        finally:
            data.close()
    except Exception as error:
        result.update(accepted_mismatched_selected_name=False,
                      error_type=type(error).__name__, message=str(error))
    results.append(result)
(a.destination/'results.json').write_text(json.dumps(results, indent=2)+'\n')
print(json.dumps(results, indent=2))
