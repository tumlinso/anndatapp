#!/usr/bin/env python3
"""Create an identity-bound, serial dataset screen plan after both builds exist."""
import argparse
import hashlib
import json
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--bbe',type=Path,required=True)
p.add_argument('--candidate',type=Path,required=True)
p.add_argument('--bbe-prefix',type=Path,required=True)
p.add_argument('--candidate-prefix',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--io-observer',type=Path,required=True)
p.add_argument('--alloc-observer',type=Path,required=True)
p.add_argument('--mode',choices=('timing','diagnostic'),required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if a.output.exists(): p.error('output exists; preserve existing plan')
items=[a.bbe,a.candidate,a.bbe_prefix/'lib/libanndatapp.a',a.bbe_prefix/'lib/libanndatapp_core.a',
       a.candidate_prefix/'lib/libanndatapp.a',a.candidate_prefix/'lib/libanndatapp_core.a',
       a.fixtures/'inventory.json',a.io_observer,a.alloc_observer,
       Path('tools/format-family-screen/dataset_screen.cpp'),Path('tools/format-family-screen/CMakeLists.txt'),
       Path('benchmarks/v1/io_counters.c'),Path('benchmarks/v1/alloc_counters.cpp')]
for f in items:
    if not f.is_file(): p.error(f'missing {f}')
inventory=json.loads((a.fixtures/'inventory.json').read_text())
for name, entry in inventory['files'].items():
    path=a.fixtures/name
    with path.open('rb') as f: actual=hashlib.file_digest(f,'sha256').hexdigest()
    if path.stat().st_size != entry['bytes'] or actual != entry['sha256']:
        p.error(f'fixture identity mismatch: {path}')
env={} if a.mode=='timing' else {'LD_PRELOAD':f'{a.alloc_observer.resolve()}:{a.io_observer.resolve()}'}
cases=[]
for n in (100000,10000):
    manifest=str((a.fixtures/str(n)/'collection.h5ads').resolve())
    for case in ('end-to-end','prepared'):
        cmds=[{'version':label,'argv':[str(exe.resolve()),case,manifest,a.mode],'env':env}
              for label,exe in (('bbe-legacy',a.bbe),('candidate-prepared',a.candidate))]
        cases.append({'name':f'{n}-{case}', 'commands':cmds})
    cases.append({'name':f'{n}-preflight','commands':[{'version':'candidate-prepared',
        'argv':[str(a.candidate.resolve()),'preflight',manifest,a.mode],'env':env}]})
plan={'description':'For prepared case legacy read includes its own preparation; compare end-to-end for equivalent work, not prepared-operation ratios.',
      'identity_files':[str(f.resolve()) for f in items]+[str(Path('build/format-family-io/prefixes-ready.json').resolve())], 'cases':cases}
a.output.parent.mkdir(parents=True,exist_ok=True)
with a.output.open('x') as f: json.dump(plan,f,indent=2);f.write('\n')
