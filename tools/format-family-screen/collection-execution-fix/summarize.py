#!/usr/bin/env python3
"""Verify frozen receipts and summarize paired results without running readers."""
import hashlib
import json
from pathlib import Path
import statistics
import sys

e=Path(sys.argv[1]).absolute();state=json.loads((e/'state.json').read_text())
if any(w['status']!='complete' for w in state['windows'].values()):raise RuntimeError('window incomplete')
def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
frozen=json.loads((e/'frozen-inputs.json').read_text())
if sha(e/'frozen-inputs.json')!=state['frozen_inputs_sha256']:raise RuntimeError('sealed input manifest changed')
for name,x in frozen.items():
    p=Path(name)
    if p.stat().st_size!=x['bytes'] or sha(p)!=x['sha256']:raise RuntimeError('frozen input changed: '+name)
prior=json.loads((e.parent/'collection-matrix-2026-11-17/summary.json').read_text())
windows={}; timings={}; diagnostics={}; digests={}; widths={};successful=0
for window in ('screen','repeat'):
    plan_path=e/(window+'-plan.json');plan=json.loads(plan_path.read_text())
    receipt=e/(window+'-receipt.jsonl');rows=[json.loads(x) for x in receipt.read_text().splitlines()]
    header=rows[0]['metadata'];footer=rows[-1]
    if state['plan_sha256'][window]!=sha(plan_path) or header['plan_sha256']!=sha(plan_path) or footer.get('status')!='completed_schedule' or footer['seconds']>30:
        raise RuntimeError('invalid or incomplete window: '+window)
    if state['windows'][window]['wall_seconds']>30:raise RuntimeError('wall cap exceeded: '+window)
    for name,digest in header['identity_sha256'].items():
        if sha(name)!=digest:raise RuntimeError('receipt input changed: '+name)
    expected={(case['name'],cmd['version']):cmd for case in plan['cases'] for cmd in case['commands']}
    records=rows[1:-1]
    if len(records)!=len(expected) or {(x.get('case'),x.get('version')) for x in records}!=set(expected):raise RuntimeError('schedule differs: '+window)
    windows[window]={'processes':len(records),'measured_seconds':footer['seconds'],'wall_seconds':state['windows'][window]['wall_seconds'],
                     'cpu':header['cpu'],'thread_limits':header['thread_limits'],'cache':header['cache'],
                     'plan_sha256':sha(plan_path),'receipt_sha256':sha(receipt)}
    for record in records:
        cmd=expected[(record['case'],record['version'])]
        if record['argv']!=cmd['argv'] or record['env_overrides']!=cmd.get('env',{}):raise RuntimeError('command differs')
        if record.get('status')!='completed' or record.get('returncode')!=0:raise RuntimeError('failed process: '+record['case'])
        row=json.loads(record['stdout']); diagnostic=record['case'].endswith('-diagnostic')
        name=record['case'].removesuffix('-diagnostic') if diagnostic else record['case'].rsplit('@',1)[0]
        version=record['version'];expected_digest=prior['cases'][name]['output_fnv1a64']
        components={'components-1':1,'components-32':32,'reverse-duplicates':8,'reuse-sparse':8,
                    'reuse-higher-density':8,'contiguous-one-component':8,'gzip-spread':8}
        if row.get('case')!=name or (version!='python' and row.get('components')!=components[name]):
            raise RuntimeError('returned case identity differs: '+name+'/'+version)
        if row.get('golden')!='pass' or row['output_fnv1a64']!=expected_digest:raise RuntimeError('oracle or digest failure: '+name+'/'+version)
        if tuple(row['shape'])!=tuple(prior['cases'][name]['modes']['default']['shape']) or row['nnz']!=prior['cases'][name]['modes']['default']['nnz']:
            raise RuntimeError('output shape or count differs: '+name+'/'+version)
        successful+=1;digests[name]=expected_digest
        if diagnostic:
            expected_phases={'open','selection','prepare','allocate','preflight','first_read','repeat_1','repeat_2','repeat_3'}
            if set(row['phases'])!=expected_phases:raise RuntimeError('diagnostic phase set differs: '+name+'/'+version)
            phases={}
            for phase,item in row['phases'].items():
                c=item['counters'];md=c['metadata_slots'];io=c['v1_io_slots']
                phases[phase]={'h5_object_opens':md[0],'h5_dataset_get_type':md[1],'h5_dataset_get_space':md[2],
                  'h5_dataset_get_create_plist':md[3],'h5_attribute_reads':md[4],'h5_group_opens':md[5],
                  'h5_string_reads':md[6],'h5dread_calls':io[14],'observed_read_calls':io[0],'observed_read_bytes':io[1],
                  'dataset_opens':item['dataset_opens'],'dataset_closes':item['dataset_closes'],
                  'cpp_new_calls':c['cpp_new_calls'],'cpp_new_bytes':c['cpp_new_bytes']}
            diagnostics.setdefault(name,{})[version]=phases;continue
        timing=timings.setdefault(name,{}).setdefault(version,[]);timing.append(row)
        width=(row.get('value_dtype'),row.get('actual_index_dtype'),row.get('actual_offset_dtype'))
        key=name+'/'+version
        if key in widths and widths[key]!=width:raise RuntimeError('native output width changed across observations')
        widths[key]=width

def distribution(values):
    return {'samples_ms':[x/1e6 for x in values],'median_ms':statistics.median(values)/1e6,
            'min_ms':min(values)/1e6,'max_ms':max(values)/1e6}

def change(before,after):
    delta=after-before
    return {'baseline_ms':before,'candidate_ms':after,'candidate_over_baseline':after/before if before else None,
            'delta_ms':delta,'material':abs(delta)>0.1 and abs(delta)/before>0.10 if before else False}

cases={};primary={'components-32','reverse-duplicates','reuse-sparse','reuse-higher-density'}
for name,modes in timings.items():
    expected_n=3 if name in primary else 1
    if set(modes)!={'baseline','candidate','python'}:raise RuntimeError('missing timing mode: '+name)
    if any(len(samples)!=expected_n for samples in modes.values()):raise RuntimeError('wrong sample count: '+name)
    if widths[name+'/baseline']!=widths[name+'/candidate']:raise RuntimeError('C++ output dtype/width changed: '+name)
    result={'digest':digests[name],'observations':expected_n,'modes':{}}
    for version,samples in modes.items():
        out={'end_to_end':distribution([x['end_to_end_ns'] for x in samples]),
             'open':distribution([x['open_ns'] for x in samples]),'native_dtypes':widths[name+'/'+version]}
        for key in ('selection_ns','preparation_requirements_ns','buffer_allocation_ns','transfer_ns','operation_ns'):
            if key in samples[0]:out[key.removesuffix('_ns')]=distribution([x[key] for x in samples])
        if name in primary:
            if any(len(x.get('repeat_transfer_ns',[]))!=4 for x in samples):raise RuntimeError('repeat count differs: '+name+'/'+version)
            out['reused_operation']=distribution([statistics.median(x['repeat_transfer_ns']) for x in samples])
            out['reused_operation']['within_process_samples_ms']=[[v/1e6 for v in x['repeat_transfer_ns']] for x in samples]
        result['modes'][version]=out
    base=result['modes']['baseline'];candidate=result['modes']['candidate'];python=result['modes']['python']
    result['first_read_change']=change(base['end_to_end']['median_ms'],candidate['end_to_end']['median_ms'])
    result['candidate_over_python_first_read']=candidate['end_to_end']['median_ms']/python['end_to_end']['median_ms']
    if name in primary:
        result['reuse_change']=change(base['reused_operation']['median_ms'],candidate['reused_operation']['median_ms'])
        result['candidate_over_python_reuse']=candidate['reused_operation']['median_ms']/python['reused_operation']['median_ms']
    cases[name]=result
if len(cases)!=7 or successful!=53 or set(diagnostics)!=primary:raise RuntimeError('case/process set differs')
if any(set(x)!={'baseline','candidate'} for x in diagnostics.values()):raise RuntimeError('missing paired diagnostic')
out={'schema':'anndatapp.collection-execution-comparison.v1','baseline_source_commit':'5d212328e10a54dea8a8918506bd39585af859d5',
     'baseline_library_commit':'d479d5d9e0cac161ff5aecb1d4eb0ff84a47836d','baseline_archive_sha256':state['baseline_archive_sha256'],
     'baseline_core_archive_sha256':state['core_archive_sha256'],'candidate_archive_sha256':state['candidate_archive_sha256'],
     'candidate_core_archive_sha256':state['candidate_core_archive_sha256'],'frozen_files':len(frozen),'successful_processes':successful,
     'windows':windows,'cases':cases,'diagnostics':diagnostics,
     'limitations':['Three independent process samples for four primary cases; controls have one. No tail-latency claim.',
       'Fresh-process normal caches; no cache eviction, exclusive-host guarantee, physical-I/O counter, or general speed claim.',
       'Each output is fully checked against the existing independent formula and cross-language canonical digest; native CSR widths/order can differ.',
       'Default C++ preserves numerical validation and destination preflight, while Python exposes a different API/validation contract.',
       'Repeated C++ operations reuse prepared plan/caller buffers; Python native indexing allocates each result. This compares lifecycles, not identical kernels.',
       'Diagnostics are separately interposed; counts exclude non-C++ allocators and physical device I/O, and instrumented durations are not timing evidence.']}
with (e/'summary.json').open('x') as f:json.dump(out,f,indent=2);f.write('\n')
print(json.dumps({'windows':windows,'successful_processes':successful,'cases':{n:{'first':v['first_read_change'],'reuse':v.get('reuse_change'),
      'candidate_over_python':v['candidate_over_python_first_read'],'candidate_over_python_reuse':v.get('candidate_over_python_reuse')} for n,v in cases.items()}},indent=2))
