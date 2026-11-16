#!/usr/bin/env python3
"""Freeze a single serial before/after collection measurement plan; never run it."""
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);p.add_argument('--candidate-prefix',type=Path,required=True);p.add_argument('--candidate-exe',type=Path,required=True);a=p.parse_args()
r=Path.cwd();d=a.evidence.absolute();prior=r/'build/release11-performance-2026-11-16';old=json.loads((prior/'input-identities.json').read_text());probe=r/'tools/format-family-screen/release11/python_probe.py';py=r/'build/reference/full/bin/python3';small=r/'build/format-family-io/fixtures-family-v3';large=r/'build/format-family-repair/screen/fixtures/10000';before=prior/'frozen-prefix';cand=a.candidate_prefix.absolute();candidate=a.candidate_exe.absolute();baseline=d/'baseline-consumer/release11-cpp';obs=r/'build/collection-preparation-2026-11-16/candidate-profile-v2/build';io=obs/'libcollection-io.so';alloc=obs/'libcollection-alloc.so';meta=d/'libmetadata.so';
def digest(path):
 with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
for name, expected in [('libanndatapp.a','807e0551bc20c68d568c171387405de9b946f4b69520a7f02b7edae08f3ad66b'),('libanndatapp_core.a','2b37603ef8c56078002abf05c5dd7ba2a295196ffe15bae5ed45d2554caeef19')]:
 if digest(before/'lib'/name)!=expected:raise RuntimeError('baseline archive differs from qualified identity: '+name)
fixtures=[small/'dataset.h5ads',*sorted(small.glob('dataset-part-*.h5ad')),large/'collection.h5ads',*sorted(large.glob('part-*.h5ad'))]
if len(fixtures)!=12:raise RuntimeError('fixture count differs')
for f in fixtures:
 entry=old[str(f)];
 if f.stat().st_size!=entry['bytes'] or digest(f)!=entry['sha256']: raise RuntimeError('fixture differs: '+str(f))
ids=[*fixtures,probe,r/'tools/format-family-screen/python_family.py',r/'tools/format-family-screen/window.py',py,baseline,candidate,meta,io,alloc,d/'source/cpp_probe.cpp',d/'source/metadata.c',d/'source/CMakeLists.txt',d/'freeze.py',r/'tools/format-family-screen/collection-fix/cpp_probe.cpp',r/'tools/format-family-screen/collection-fix/metadata.c',r/'tools/format-family-screen/collection-fix/CMakeLists.txt',r/'tools/format-family-screen/collection-fix/freeze.py',r/'tools/format-family-screen/collection-fix/summarize.py',before/'lib/libanndatapp.a',before/'lib/libanndatapp_core.a',cand/'lib/libanndatapp.a',cand/'lib/libanndatapp_core.a']
pre=':'.join(map(str,(meta,io,alloc)));cases=[]
def cmd(v,case,fixture,diag=False):
 argv=[str(py),str(probe),case,str(fixture)] if v=='python' else [str(baseline if v=='baseline' else candidate),case,str(fixture)]
 if diag:argv.append('--diagnostic')
 env={'ANNDATAPP_INTEROP_ZARR_WAKEUP':'0'}
 if diag:env['LD_PRELOAD']=pre
 return {'version':v,'argv':argv,'env':env}
for repeat in range(1,4):
 for name,fixture in [('dataset-small',small/'dataset.h5ads'),('dataset-10k',large/'collection.h5ads')]:
  cases.append({'name':f'{name}@{repeat}','commands':[cmd(v,name,fixture) for v in ('baseline','candidate','python')]})
for name,fixture in [('dataset-small',small/'dataset.h5ads'),('dataset-10k',large/'collection.h5ads')]:
 cases.append({'name':name+'-diagnostic','commands':[cmd(v,name,fixture,True) for v in ('baseline','candidate')]})
plan={'description':'One 30-second aggregate window; three paired Release timing observations and one separate interposed diagnostic per C++ build and geometry; Python uses native simultaneous X indexing without preload.','identity_files':[str(f) for f in ids],'cases':cases}
out=d/'plan.json'
with out.open('x') as f:json.dump(plan,f,indent=2);f.write('\n')
print('Frozen',len(cases),'cases,',sum(len(c['commands']) for c in cases),'processes at',out)
