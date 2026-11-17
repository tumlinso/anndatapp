#!/usr/bin/env python3
"""Freeze the two approved paired collection execution comparison windows."""
import argparse
import hashlib
import json
import os
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--candidate-prefix',type=Path,required=True);a=p.parse_args()
r=Path.cwd();e=r/'build/collection-execution-fix-2026-11-17';prefix=a.candidate_prefix.absolute()
prior=r/'build/collection-matrix-2026-11-17';fixture_root=prior/'fixtures-complete';source=e/'source'
py=r/'build/reference/full/bin/python3'  # Retain the virtualenv lexical path.
cases=['components-32','reverse-duplicates','reuse-sparse','reuse-higher-density','components-1','contiguous-one-component','gzip-spread']
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
baseline=json.loads((e/'baseline-frozen.json').read_text())
for name,x in baseline.items():
    path=Path(name)
    if path.stat().st_size!=x['bytes'] or sha(path)!=x['sha256']:raise RuntimeError('frozen baseline changed: '+name)
old=json.loads((prior/'frozen-inputs.json').read_text())
candidate_manifest=e/'candidate-source-identities.json'
candidate_source=json.loads(candidate_manifest.read_text())
for name,x in candidate_source.items():
    path=r/name
    if path.stat().st_size!=x['bytes'] or sha(path)!=x['sha256']:
        raise RuntimeError('candidate source changed since release build: '+name)
case_files=[prior/'cases'/f'{name}.{suffix}' for name in cases for suffix in ('txt','json')]
fixtures=[x for name in ('sparse-c1','sparse-c8','sparse-c32','higher-density-c8','gzip-c8') for x in sorted((fixture_root/name).glob('*.h5ad*'))]
for path in case_files+fixtures:
    entry=old[str(path)]
    if path.stat().st_size!=entry['bytes'] or sha(path)!=entry['sha256']:raise RuntimeError('fixture or selector changed: '+str(path))
def write_once(path,content):
    if path.exists():
        if path.read_text()!=content:raise RuntimeError('existing artifact differs; preserve it: '+str(path))
        return
    with path.open('x') as f:f.write(content);f.flush();os.fsync(f.fileno())

case_root=e/'cases';case_root.mkdir(exist_ok=True)
allowed={f'{name}.{suffix}' for name in cases for suffix in ('txt','json')}
if any(path.name not in allowed for path in case_root.iterdir()):
    raise RuntimeError('unexpected pre-existing case artifact; preserve it')
for name in cases:
    repeat=5 if name in cases[:4] else 1
    text=(prior/'cases'/f'{name}.txt').read_text().splitlines()
    nr,nc,_=text[3].split();text[3]=f'{nr} {nc} {repeat}'
    write_once(case_root/f'{name}.txt','\n'.join(text)+'\n')
    item=json.loads((prior/'cases'/f'{name}.json').read_text());item['repeat']=repeat
    write_once(case_root/f'{name}.json',json.dumps(item,indent=2)+'\n')
frozen_cases=sorted(case_root.iterdir())
candidate_files=[prefix/'lib/libanndatapp.a',prefix/'lib/libanndatapp_core.a',*sorted((prefix/'share/anndatapp/modules').glob('*.cppm')),
                 e/'candidate-consumer/execution-time',e/'candidate-consumer/execution-count',e/'candidate-consumer/CMakeCache.txt',
                 e/'baseline-link.txt',e/'candidate-link.txt']
script_dir=r/'tools/format-family-screen/collection-execution-fix'
items=[*[Path(x) for x in baseline],*candidate_files,*[r/name for name in candidate_source],
       candidate_manifest,e/'candidate-source.patch',e/'build-release-sealed.log',e/'install-release.log',
       *case_files,*frozen_cases,*fixtures,prior/'summary.json',prior/'frozen-inputs.json',
       prior/'environment.json',py,*sorted((r/'build/reference/full/lib/python3.12/site-packages/snapatac2').glob('*.so')),
       *sorted(x for x in script_dir.iterdir() if x.is_file()),r/'tests/reference/requirements-full.lock']
for path in items:
    if not path.is_file():raise RuntimeError('missing identity input: '+str(path))
preload=':'.join(str(e/'interposers'/n) for n in ('libmetadata.so','libhandles.so','libcollection-io.so','libcollection-alloc.so'))
def command(version,name,diag=False):
    binary=('execution-count' if diag else 'execution-time')
    if version=='python':argv=[str(py),str(source/'collection-matrix/python_probe.py'),str(case_root/f'{name}.json')]
    else:argv=[str(e/(version+'-consumer')/binary),str(case_root/f'{name}.txt')]
    env={'ANNDATAPP_INTEROP_ZARR_WAKEUP':'0'}
    if diag:env['LD_PRELOAD']=preload
    return {'version':version,'argv':argv,'env':env}
def write_window(window,primary,controls):
    schedule=[]
    for repeat in range(1,4):
        for name in primary:
            orders=[['baseline','candidate','python'],['python','candidate','baseline'],['candidate','python','baseline']]
            versions=orders[repeat-1].copy()
            if len(schedule)%2:versions.reverse()  # Runner reverses alternate cases.
            schedule.append({'name':f'{name}@{repeat}','commands':[command(v,name) for v in versions]})
    for name in controls:
        versions=['python','baseline','candidate']
        if len(schedule)%2:versions.reverse()
        schedule.append({'name':name+'@1','commands':[command(v,name) for v in versions]})
    for name in primary:
        versions=['baseline','candidate']
        if len(schedule)%2:versions.reverse()
        schedule.append({'name':name+'-diagnostic','commands':[command(v,name,True) for v in versions]})
    data={'description':f'One <=30s paired {window} window; serial Release consumers and SnapATAC2 on identical fixture/selector outputs',
          'identity_files':[str(x) for x in items],'cases':schedule}
    write_once(e/(window+'-plan.json'),json.dumps(data,indent=2)+'\n')
    return sum(len(c['commands']) for c in schedule)
counts={'screen':write_window('screen',cases[:2],cases[4:6]),'repeat':write_window('repeat',cases[2:4],cases[6:])}
write_once(e/'frozen-inputs.json',json.dumps({str(path):{'bytes':path.stat().st_size,'sha256':sha(path)} for path in items},indent=2)+'\n')
state_path=e/'state.json';state=json.loads(state_path.read_text())
if state['status']!='baseline_ready':raise RuntimeError('baseline state not ready')
state.update(status='ready',candidate_prefix=str(prefix),candidate_archive_sha256=sha(prefix/'lib/libanndatapp.a'),
             candidate_core_archive_sha256=sha(prefix/'lib/libanndatapp_core.a'),
             candidate_source_files=len(candidate_source),candidate_source_manifest_sha256=sha(candidate_manifest),
             processes=counts,frozen_inputs=len({str(x) for x in items}),
             plan_sha256={name:sha(e/(name+'-plan.json')) for name in ('screen','repeat')},
             frozen_inputs_sha256=sha(e/'frozen-inputs.json'))
with state_path.open('w') as f:json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
fd=os.open(e,os.O_DIRECTORY);os.fsync(fd);os.close(fd)
print(json.dumps({'status':state['status'],'processes':counts,'frozen_inputs':state['frozen_inputs'],'candidate_archive_sha256':state['candidate_archive_sha256']}))
