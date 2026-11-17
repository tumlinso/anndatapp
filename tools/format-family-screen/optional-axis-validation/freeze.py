#!/usr/bin/env python3
"""Freeze the serial one-window comparison; do not execute fixtures."""
import argparse, hashlib, json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--candidate-prefix', type=Path, required=True)
a = p.parse_args()
r = Path.cwd(); d = a.evidence.absolute(); cand = a.candidate_prefix.absolute()
prior = r/'build/release11-performance-2026-11-16'
old = json.loads((prior/'input-identities.json').read_text())
small = r/'build/format-family-io/fixtures-family-v3'
large = r/'build/format-family-repair/screen/fixtures/10000'
before = r/'build/collection-prep-fix-2026-11-16/candidate-prefix'
probe = r/'tools/format-family-screen/release11/python_probe.py'
py = r/'build/reference/full/bin/python3'  # Keep this lexical virtualenv path.
base_exe = d/'baseline-consumer/optional-axis-probe'
cand_exe = d/'candidate-consumer/optional-axis-probe'
obs = r/'build/collection-preparation-2026-11-16/candidate-profile-v2/build'
meta = r/'build/collection-prep-fix-2026-11-16/comparison/libmetadata.so'
io = obs/'libcollection-io.so'; alloc = obs/'libcollection-alloc.so'
def digest(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
for name, expected in [('libanndatapp.a','8122de5dd04065111433f305502bf408a960efbd8fcac1b8366b5a423dabaa38'), ('libanndatapp_core.a','2b37603ef8c56078002abf05c5dd7ba2a295196ffe15bae5ed45d2554caeef19')]:
    if digest(before/'lib'/name) != expected: raise RuntimeError('baseline archive differs: '+name)
fixtures = [small/'dataset.h5ads', *sorted(small.glob('dataset-part-*.h5ad')), large/'collection.h5ads', *sorted(large.glob('part-*.h5ad'))]
if len(fixtures) != 12: raise RuntimeError('fixture count differs')
for f in fixtures:
    entry = old[str(f)]
    if f.stat().st_size != entry['bytes'] or digest(f) != entry['sha256']: raise RuntimeError('fixture differs: '+str(f))
src = r/'tools/format-family-screen/optional-axis-validation'
ids = [*fixtures, probe, r/'tools/format-family-screen/python_family.py', r/'tools/format-family-screen/window.py', py, base_exe, cand_exe, meta, io, alloc, src/'cpp_probe.cpp', src/'CMakeLists.txt', src/'freeze.py', src/'summarize.py', r/'tools/format-family-screen/collection-fix/cpp_probe.cpp', before/'lib/libanndatapp.a', before/'lib/libanndatapp_core.a', cand/'lib/libanndatapp.a', cand/'lib/libanndatapp_core.a']
pre = ':'.join(map(str, (meta, io, alloc)))
def cmd(v, case, fixture, diag=False):
    argv = [str(py), str(probe), case, str(fixture)] if v == 'python' else [str(base_exe if v == 'baseline' else cand_exe), case, str(fixture)]
    if v == 'checked': argv.append('--check-axis-names')
    if diag: argv.append('--diagnostic')
    env = {'ANNDATAPP_INTEROP_ZARR_WAKEUP':'0'}
    if diag: env['LD_PRELOAD'] = pre
    return {'version':v, 'argv':argv, 'env':env}
cases = []
for repeat in range(1,4):
    for name, fixture in [('dataset-small',small/'dataset.h5ads'), ('dataset-10k',large/'collection.h5ads')]:
        # Counteract the runner's alternating case reversal so both geometries alternate by repetition.
        versions = ('baseline','default','checked','python')
        if (repeat % 2 == 0) != (name == 'dataset-10k'): versions = tuple(reversed(versions))
        cases.append({'name':f'{name}@{repeat}', 'commands':[cmd(v,name,fixture) for v in versions]})
for name,fixture in [('dataset-small',small/'dataset.h5ads'), ('dataset-10k',large/'collection.h5ads')]:
    cases.append({'name':name+'-diagnostic', 'commands':[cmd(v,name,fixture,True) for v in ('baseline','default','checked')]})
plan = {'description':'One <=30 s serial window: three repetitions of four modes per geometry, then six separately interposed C++ diagnostics.', 'identity_files':[str(f) for f in ids], 'cases':cases}
with (d/'plan.json').open('x') as f: json.dump(plan,f,indent=2); f.write('\n')
print('Frozen',len(cases),'cases,',sum(len(c['commands']) for c in cases),'processes')
