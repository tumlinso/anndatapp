#!/usr/bin/env python3
"""Freeze matched 22-process collection experiment. Does not run fixtures."""
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--repo',type=Path,default=Path.cwd())
p.add_argument('--reference-root',type=Path,required=True)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--candidate-prefix',type=Path,required=True)
a=p.parse_args(); new=a.repo.absolute(); old=a.reference_root.absolute(); d=a.evidence.absolute()
prior = old/'build/release11-performance-2026-11-16/input-identities.json'
small = old/'build/format-family-io/fixtures-family-v3'; large = old/'build/format-family-repair/screen/fixtures/10000'
base = old/'build/optional-axis-validation-2026-11-17/candidate-prefix'; cand = a.candidate_prefix.absolute()
py = old/'build/reference/full/bin/python3'; python_probe = old/'tools/format-family-screen/release11/python_probe.py'
meta = old/'build/collection-prep-fix-2026-11-16/comparison/libmetadata.so'
obs = old/'build/collection-preparation-2026-11-16/candidate-profile-v2/build'
libs = [meta,obs/'libcollection-io.so',obs/'libcollection-alloc.so']
def sha(p):
 with p.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(base/'lib/libanndatapp.a') == '2678592fc437c3744915c0672943c458a7b7dd2840e113d1f1a4a67abe4be930'
assert sha(base/'lib/libanndatapp_core.a') == '2b37603ef8c56078002abf05c5dd7ba2a295196ffe15bae5ed45d2554caeef19'
fixtures = [small/'dataset.h5ads', *sorted(small.glob('dataset-part-*.h5ad')),large/'collection.h5ads',*sorted(large.glob('part-*.h5ad'))]
if len(fixtures)!=12: raise RuntimeError('fixture count differs')
previous=json.loads(prior.read_text())
for f in fixtures:
 entry=previous[str(f)]
 if f.stat().st_size != entry['bytes'] or sha(f)!=entry['sha256']: raise RuntimeError('fixture changed: '+str(f))
source=d/'source'; tool=new/'tools/format-family-screen/metadata-reuse'
executables=[d/(v+'-consumer')/n for v in ('baseline','candidate') for n in ('small-collection-cpp','collection-control-cpp')]
ids=[*fixtures,*executables,py,python_probe,old/'tools/format-family-screen/python_family.py',old/'tools/format-family-screen/window.py',*libs,source/'small_collection_cpp.cpp',source/'reference_cpp_probe.cpp',source/'control_cpp.cpp',source/'CMakeLists.txt',tool/'freeze.py',tool/'summarize.py',tool/'setup.py',tool/'small_collection_cpp.cpp',tool/'control_cpp.cpp',tool/'CMakeLists.txt',new/'tools/format-family-screen/collection-fix/cpp_probe.cpp',base/'lib/libanndatapp.a',base/'lib/libanndatapp_core.a',cand/'lib/libanndatapp.a',cand/'lib/libanndatapp_core.a',prior]
pre=':'.join(map(str,libs))
def command(version,case,fixture,diag=False):
 if version=='python': argv=[str(py),str(python_probe),case,str(fixture)]
 else:
  exe=d/(version+'-consumer')/('small-collection-cpp' if case=='dataset-small' else 'collection-control-cpp')
  argv=[str(exe),str(fixture)] if case=='dataset-small' else [str(exe),case,str(fixture)]
  if diag: argv.append('--diagnostic')
 env={'ANNDATAPP_INTEROP_ZARR_WAKEUP':'0'}
 if diag:env['LD_PRELOAD']=pre
 return {'version':version,'argv':argv,'env':env}
cases=[]
for rep in range(1,4):
 for name,fixture in [('dataset-small',small/'dataset.h5ads'),('dataset-10k',large/'collection.h5ads')]:
  versions=('baseline','candidate','python') if rep!=2 else ('python','candidate','baseline')
  cases.append({'name':f'{name}@{rep}','commands':[command(v,name,fixture) for v in versions]})
for name,fixture in [('dataset-small',small/'dataset.h5ads'),('dataset-10k',large/'collection.h5ads')]:
 cases.append({'name':name+'-diagnostic','commands':[command(v,name,fixture,True) for v in ('baseline','candidate')]})
plan={'description':'One <=30s serial window: three matched baseline/candidate/Python timing observations per geometry plus four separate C++ diagnostics.','identity_files':[str(x) for x in ids],'cases':cases}
with (d/'plan.json').open('x') as f: json.dump(plan,f,indent=2);f.write('\n')
print('Frozen 22 processes; candidate archive sha256',sha(cand/'lib/libanndatapp.a'))
