#!/usr/bin/env python3
"""Inspect the completed first window without starting any additional reader."""
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys

e=Path(sys.argv[1]).absolute();state=json.loads((e/'state.json').read_text())
if state['windows']['screen']['status']!='complete':raise RuntimeError('screen incomplete')
def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
frozen=json.loads((e/'frozen-inputs.json').read_text())
if sha(e/'frozen-inputs.json')!=state['frozen_inputs_sha256']:raise RuntimeError('sealed input manifest changed')
for name,x in frozen.items():
    p=Path(name)
    if p.stat().st_size!=x['bytes'] or sha(p)!=x['sha256']:raise RuntimeError('frozen input changed: '+name)
plan_path=e/'screen-plan.json';plan=json.loads(plan_path.read_text());receipt=e/'screen-receipt.jsonl'
rows=[json.loads(line) for line in receipt.read_text().splitlines()];header=rows[0]['metadata'];footer=rows[-1]
if state['plan_sha256']['screen']!=sha(plan_path) or header['plan_sha256']!=sha(plan_path) or footer.get('status')!='completed_schedule' or footer['seconds']>30 or state['windows']['screen']['wall_seconds']>30:
    raise RuntimeError('screen failed or exceeded cap')
for name,digest in header['identity_sha256'].items():
    if sha(name)!=digest:raise RuntimeError('receipt input changed: '+name)
expected={(case['name'],cmd['version']):cmd for case in plan['cases'] for cmd in case['commands']};records=rows[1:-1]
if len(records)!=len(expected) or {(x.get('case'),x.get('version')) for x in records}!=set(expected):raise RuntimeError('screen schedule differs')
prior=json.loads((e.parent/'collection-matrix-2026-11-17/summary.json').read_text())
timings={};diagnostics={}
for record in records:
    cmd=expected[(record['case'],record['version'])]
    if record['argv']!=cmd['argv'] or record['env_overrides']!=cmd.get('env',{}):raise RuntimeError('command differs')
    if record.get('status')!='completed' or record.get('returncode')!=0:raise RuntimeError('screen process failed')
    row=json.loads(record['stdout']);is_diag=record['case'].endswith('-diagnostic')
    name=record['case'].removesuffix('-diagnostic') if is_diag else record['case'].rsplit('@',1)[0]
    components={'components-1':1,'components-32':32,'reverse-duplicates':8,'contiguous-one-component':8}
    if row.get('case')!=name or (record['version']!='python' and row.get('components')!=components[name]):
        raise RuntimeError('returned case identity differs: '+name)
    if row.get('golden')!='pass' or row['output_fnv1a64']!=prior['cases'][name]['output_fnv1a64']:
        raise RuntimeError('oracle or digest failure: '+name)
    if row['shape']!=prior['cases'][name]['modes']['default']['shape'] or row['nnz']!=prior['cases'][name]['modes']['default']['nnz']:
        raise RuntimeError('shape or nnz differs: '+name)
    if is_diag:
        expected_phases={'open','selection','prepare','allocate','preflight','first_read','repeat_1','repeat_2','repeat_3'}
        if set(row['phases'])!=expected_phases:raise RuntimeError('diagnostic phase set differs: '+name)
        phases={}
        for phase in ('prepare','preflight','first_read','repeat_1','repeat_2','repeat_3'):
            c=row['phases'][phase]['counters'];phases[phase]={'h5_object_opens':c['metadata_slots'][0],
               'h5dread_calls':c['v1_io_slots'][14],'observed_read_calls':c['v1_io_slots'][0],
               'observed_read_bytes':c['v1_io_slots'][1],'cpp_new_calls':c['cpp_new_calls'],'cpp_new_bytes':c['cpp_new_bytes']}
        diagnostics.setdefault(name,{})[record['version']]=phases
    else:timings.setdefault(name,{}).setdefault(record['version'],[]).append(row)
def median_ms(values):return statistics.median(values)/1e6
out={};primary={'components-32','reverse-duplicates'}
for name,versions in timings.items():
    n=3 if name in primary else 1
    if set(versions)!={'baseline','candidate','python'} or any(len(v)!=n for v in versions.values()):raise RuntimeError('sample count differs')
    widths=lambda row:(row['value_dtype'],row['actual_index_dtype'],row['actual_offset_dtype'])
    if widths(versions['baseline'][0])!=widths(versions['candidate'][0]):raise RuntimeError('C++ width mismatch')
    modes={}
    for version,samples in versions.items():
        if any(widths(x)!=widths(samples[0]) for x in samples):raise RuntimeError('width changed across samples')
        modes[version]={'first_ms':median_ms([x['end_to_end_ns'] for x in samples]),'dtypes':widths(samples[0])}
        if name in primary:
            if any(len(x['repeat_transfer_ns'])!=4 for x in samples):raise RuntimeError('repeat count differs')
            modes[version]['repeated_ms']=median_ms([statistics.median(x['repeat_transfer_ns']) for x in samples])
    before=modes['baseline']['first_ms'];after=modes['candidate']['first_ms']
    entry={'observations':n,'modes':modes,'candidate_over_baseline':after/before,
           'first_material_regression':after-before>0.1 and (after-before)/before>0.1}
    if name in primary:
        b=modes['baseline']['repeated_ms'];a=modes['candidate']['repeated_ms']
        entry.update(repeat_candidate_over_baseline=a/b,repeat_material_regression=a-b>0.1 and (a-b)/b>0.1)
    out[name]=entry
if set(out)!={'components-32','reverse-duplicates','components-1','contiguous-one-component'} or set(diagnostics)!=primary:
    raise RuntimeError('screen cases differ')
if any(set(x)!={'baseline','candidate'} for x in diagnostics.values()):raise RuntimeError('diagnostics incomplete')
result={'status':'passed','processes':len(records),'wall_seconds':state['windows']['screen']['wall_seconds'],
        'receipt_sha256':sha(receipt),'plan_sha256':sha(plan_path),'cases':out,'diagnostics':diagnostics,
        'scope':'screen evidence for explicit review; no additional readers or automatic second window'}
with (e/'screen-summary.json').open('x') as f:json.dump(result,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
print(json.dumps(result,indent=2))
