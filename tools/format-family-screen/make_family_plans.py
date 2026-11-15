#!/usr/bin/env python3
"""Create separate bounded format plans once fixtures and binaries are frozen."""
import argparse
import hashlib
import json
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--bbe',type=Path,required=True)
p.add_argument('--candidate',type=Path,required=True)
p.add_argument('--python',type=Path,required=True)
p.add_argument('--bbe-prefix',type=Path,required=True)
p.add_argument('--candidate-prefix',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--io-observer',type=Path,required=True)
p.add_argument('--alloc-observer',type=Path,required=True)
p.add_argument('--mode',choices=('timing','diagnostic'),required=True)
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args()
files=[a.bbe,a.candidate,a.python,a.bbe_prefix/'lib/libanndatapp.a',a.bbe_prefix/'lib/libanndatapp_core.a',
       a.candidate_prefix/'lib/libanndatapp.a',a.candidate_prefix/'lib/libanndatapp_core.a',
       a.io_observer,a.alloc_observer,Path('tools/format-family-screen/family_screen.cpp'),
       Path('tools/format-family-screen/python_family.py'),Path('benchmarks/v1/io_counters.c'),
       Path('benchmarks/v1/alloc_counters.cpp'),a.fixtures/'inventory.json',a.fixtures/'validation.json']
for f in files:
    if not f.is_file(): p.error(f'missing {f}')
inventory=json.loads((a.fixtures/'inventory.json').read_text())
for name,item in inventory['files'].items():
    path=a.fixtures/name
    with path.open('rb') as f: actual=hashlib.file_digest(f,'sha256').hexdigest()
    if path.stat().st_size != item['bytes'] or actual != item['sha256']:
        p.error(f'fixture identity mismatch: {path}')
fixtures={'anndata':'anndata.h5ad','mudata':'mudata.h5mu','treedata':'treedata.h5td',
          'dataset':'dataset.h5ads','spatialdata':'spatialdata.zarr'}
cases={'anndata':['anndata'], 'mudata':['mudata','mudata-map'],
       'treedata':['treedata','treedata-tree'], 'dataset':['dataset'],
       'spatialdata':['spatialdata','spatialdata-raster','spatialdata-points']}
preload=f'{a.alloc_observer.resolve()}:{a.io_observer.resolve()}'
env={} if a.mode=='timing' else {'LD_PRELOAD':preload}
extra=[] if a.mode=='timing' else ['--diagnostic']
a.output_dir.mkdir(parents=True,exist_ok=True)
for family, names in cases.items():
    output=a.output_dir/f'{family}-{a.mode}-plan.json'
    if output.exists(): p.error(f'output exists: {output}')
    path=str((a.fixtures/fixtures[family]).resolve())
    entries=[]
    for case in names:
        commands=[]
        for label,argv in [('bbe',[str(a.bbe.resolve()),case,path]),
                           ('candidate',[str(a.candidate.resolve()),case,path]),
                           ('python',[str(a.python.absolute()),str(Path('tools/format-family-screen/python_family.py').resolve()),'case',case,str(a.fixtures.resolve())])]:
            cmd_env=dict(env)
            if label == 'python': cmd_env['ANNDATAPP_INTEROP_ZARR_WAKEUP']='0'
            commands.append({'version':label,'argv':argv+extra,'env':cmd_env})
        entries.append({'name':case,'commands':commands})
    plan={'description':f'{family}: compare equivalent requested outputs and end-to-end only; eager work may differ by API.',
          'identity_files':[str(f.absolute()) for f in files]+[str(Path('build/format-family-io/prefixes-ready.json').resolve())], 'cases':entries}
    with output.open('x') as f: json.dump(plan,f,indent=2);f.write('\n')
