#!/usr/bin/env python3
"""Bind the diagnostic to the existing fixture, library, probe and interposers."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path.cwd()
e = root/'build/collection-overhead-2026-11-17'
prior = root/'build/collection-matrix-2026-11-17'
prefix = root/'build/optional-axis-validation-2026-11-17/candidate-prefix'
src = root/'tools/format-family-screen/collection-overhead'
fixture_root = prior/'fixtures-complete'
cases = ['components-1','components-8','components-32','contiguous-one-component','reverse-duplicates','reuse-higher-density']
interposers = [root/'build/collection-prep-fix-2026-11-16/comparison/libmetadata.so', e/'libhandles.so',
               root/'build/collection-preparation-2026-11-16/candidate-profile-v2/build/libcollection-io.so',
               root/'build/collection-preparation-2026-11-16/candidate-profile-v2/build/libcollection-alloc.so']

def digest(path):
    with path.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()

old = json.loads((prior/'frozen-inputs.json').read_text())
items = [prior/'cases'/f'{x}.txt' for x in cases]
fixtures = [p for part in ('sparse-c1','sparse-c8','sparse-c32','higher-density-c8') for p in sorted((fixture_root/part).glob('*.h5ad*'))]
for path in items + fixtures:
    entry = old[str(path)]
    if path.stat().st_size != entry['bytes'] or digest(path) != entry['sha256']:
        raise RuntimeError('previously frozen fixture changed: '+str(path))
archive = prefix/'lib/libanndatapp.a'
if digest(archive) != '2678592fc437c3744915c0672943c458a7b7dd2840e113d1f1a4a67abe4be930':
    raise RuntimeError('library archive does not match measured 32852b6')
source_files = sorted(p for p in src.iterdir() if p.is_file())
files = [*source_files,root/'tools/format-family-screen/collection-fix/cpp_probe.cpp',root/'tools/format-family-screen/collection-matrix/cpp_probe.cpp',
         root/'tools/format-family-screen/window.py',prior/'summary.json',prior/'frozen-inputs.json',e/'consumer/collection-overhead-cpp',
         e/'consumer/CMakeCache.txt',archive,prefix/'lib/libanndatapp_core.a',*interposers,*items,*fixtures]
files += sorted((prefix/'share/anndatapp/modules').glob('*.cppm'))
for p in files:
    if not p.is_file(): raise RuntimeError('missing input: '+str(p))
identities = {str(p):{'bytes':p.stat().st_size,'sha256':digest(p)} for p in files}
snapshot = e/'source-snapshot'; snapshot.mkdir(exist_ok=False)
for path in source_files:
    shutil.copy2(path,snapshot/path.name)
commands = []
for name,path in zip(cases,items):
    commands.append({'name':name,'commands':[{'version':'diagnostic',
        'argv':[str(e/'consumer/collection-overhead-cpp'),str(path)],
        'env':{'LD_PRELOAD':':'.join(map(str,interposers))}}]})
plan={'description':'One <=30-second serial interposed phase diagnostic, six C++ processes, normal caches; no speed measurements',
      'identity_files':[str(p) for p in files], 'cases':commands}
for path,data in ((e/'frozen-inputs.json',identities),(e/'plan.json',plan)):
    with path.open('x') as f: json.dump(data,f,indent=2);f.write('\n')
state=json.loads((e/'state.json').read_text());state.update(status='ready',cases=cases,processes=len(commands),frozen_files=len(identities))
with (e/'state.json').open('w') as f:
    json.dump(state,f,indent=2);f.write('\n');f.flush()
    import os; os.fsync(f.fileno())
print(json.dumps({'cases':cases,'processes':len(commands),'frozen_files':len(identities),'fixtures_reused':len(fixtures)}))
