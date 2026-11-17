#!/usr/bin/env python3
"""Verify frozen receipts and extract phase counters; no workloads are run."""
import hashlib
import json
from pathlib import Path
import sys

e=Path(sys.argv[1]).absolute(); plan=json.loads((e/'plan.json').read_text())
rows=[json.loads(s) for s in (e/'receipt.jsonl').read_text().splitlines()]
frozen=json.loads((e/'frozen-inputs.json').read_text())
def sha(p):
    with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
for p,v in frozen.items():
    if Path(p).stat().st_size!=v['bytes'] or sha(p)!=v['sha256']:raise RuntimeError('input differs: '+p)
meta=rows[0]['metadata']; footer=rows[-1]
if meta['plan_sha256']!=sha(e/'plan.json') or footer.get('status')!='completed_schedule' or footer['seconds']>30:
    raise RuntimeError('invalid receipt or exceeded cap')
for p,v in meta['identity_sha256'].items():
    if sha(p)!=v:raise RuntimeError('receipt identity differs: '+p)
expected={(case['name'],c['version']) for case in plan['cases'] for c in case['commands']}
records=rows[1:-1]
if len(records)!=len(expected) or {(r.get('case'),r.get('version')) for r in records}!=expected:
    raise RuntimeError('missing/duplicate process')
prior=json.loads((e.parent/'collection-matrix-2026-11-17/summary.json').read_text())
result={}
for record in records:
    if record.get('status')!='completed' or record.get('returncode')!=0:raise RuntimeError('failed process: '+record['case'])
    row=json.loads(record['stdout']);name=record['case']
    if row['golden']!='pass' or row['output_fnv1a64']!=prior['cases'][name]['output_fnv1a64']:
        raise RuntimeError('oracle/digest differs: '+name)
    phases={}
    for phase,item in row['phases'].items():
        c=item['counters'];md=c['metadata_slots'];io=c['v1_io_slots']
        phases[phase]={'dataset_opens':item['dataset_opens'],'dataset_closes':item['dataset_closes'],
           'h5_object_opens':md[0],'h5_dataset_get_type':md[1],'h5_dataset_get_space':md[2],
           'h5_dataset_get_create_plist':md[3],'h5_attribute_reads':md[4],'h5_group_opens':md[5],
           'h5_string_reads':md[6],'h5dread_calls':io[14],
           'observed_read_calls':io[0],'observed_read_bytes':io[1],
           'cpp_new_calls':c['cpp_new_calls'],'cpp_new_bytes':c['cpp_new_bytes']}
    result[name]={'components':row['components'],'shape':row['shape'],'nnz':row['nnz'],
                  'output_fnv1a64':row['output_fnv1a64'],'phases':phases}
out={'source_commit':'7f8d72af02cd5b5b1cfc7de3c019658baa6e230b','measured_library_commit':'32852b6b65d0ab6d8204f3cb5ebb7cc584be0def',
     'window_seconds':footer['seconds'],'runner_wall_seconds':json.loads((e/'state.json').read_text())['runner_wall_seconds'],
     'cpu':meta['cpu'],'cache':meta['cache'],'thread_limits':meta['thread_limits'],'frozen_files':len(frozen),
     'plan_sha256':sha(e/'plan.json'),'receipt_sha256':sha(e/'receipt.jsonl'),
     'cases':result,'scope':'Separate interposed diagnostic; one process per case, no timings or physical-I/O claims. Read phases include whole-plan preflight.'}
with (e/'summary.json').open('x') as f:json.dump(out,f,indent=2);f.write('\n')
print(json.dumps(out,indent=2))
