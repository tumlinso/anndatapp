#!/usr/bin/env python3
"""Verify the frozen one-window receipt and summarize without running fixtures."""
import argparse,hashlib,json,statistics
from pathlib import Path
from collections import defaultdict
p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);base=p.parse_args().evidence.absolute()
plan=json.loads((base/'plan.json').read_text());lines=[json.loads(x) for x in (base/'receipt.jsonl').read_text().splitlines()];meta=lines[0]['metadata'];end=lines[-1]
if end.get('status')!='completed_schedule' or end['seconds']>=30:raise RuntimeError('incomplete or over-budget window')
for name,expected in meta['identity_sha256'].items():
 with open(name,'rb') as f: actual=hashlib.file_digest(f,'sha256').hexdigest()
 if actual!=expected:raise RuntimeError('input changed: '+name)
expected=[(case['name'],c['version']) for case in plan['cases'] for c in case['commands']];records=[r for r in lines if 'version' in r]
if len(records)!=len(expected) or set((r['case'],r['version']) for r in records)!=set(expected):raise RuntimeError('schedule incomplete or duplicated')
times=defaultdict(list);diags={};hashes=defaultdict(set);metadata={}
for r in records:
 if r['status']!='completed':raise RuntimeError(f"process failed {r['case']} {r['version']}: {r.get('stderr')}")
 o=json.loads(r['stdout']);case=r['case'].split('@')[0].removesuffix('-diagnostic');out=o.get('output',o)
 if o.get('golden')!='pass':raise RuntimeError('oracle failure')
 hashes[case].add(out['output_fnv1a64']);metadata[case+'/'+r['version']]={k:out[k] for k in ('shape','nnz','value_dtype','actual_index_dtype','actual_offset_dtype','native_dtypes') if k in out}
 if r['case'].endswith('-diagnostic'):
  diags[case+'/'+r['version']]=o['diagnostic'];continue
 times[(case,r['version'])].append(o)
if any(len(v)!=1 for v in hashes.values()):raise RuntimeError('output digest disagreement')
for case, checked_strings in [('dataset-small',6),('dataset-10k',18)]:
 for mode in ('baseline','default','checked'):
  phase=diags[case+'/'+mode]['phases']['preparation_requirements']
  strings=phase['metadata_slots'][6]; total=phase['v1_io_slots'][14]
  if total-strings != 128 or strings != (0 if mode=='default' else checked_strings):
   raise RuntimeError(f'unexpected preparation H5Dread counts {case}/{mode}: total={total}, strings={strings}')
result={'window_seconds':end['seconds'],'record_count':len(records),'settings':{k:meta[k] for k in ['cpu','thread_limits','cache','case_timeout_seconds','plan_sha256','runner_sha256']},'hashes':{k:next(iter(v)) for k,v in hashes.items()},'metadata':metadata,'timings':{},'comparisons':{},'diagnostics':diags,'caveats':['Python native X selection does not perform the same selected-axis validation as baseline and checked C++; compare boundaries with care.','Diagnostics count intercepted calls and bytes, not physical I/O; C++ new excludes other allocators. String read counts classify H5Dread memory types.','Normal caches; three fresh-process observations per geometry and version.']}
for (case,version),rows in times.items():
 if len(rows)!=3:raise RuntimeError('incorrect number of timing samples')
 keys=[k for k in ('open_ns','selection_ns','preparation_requirements_ns','buffer_allocation_ns','transfer_ns','operation_ns','end_to_end_ns') if k in rows[0]]
 result['timings'][case+'/'+version]={k:{'samples':[r[k] for r in rows],'median':statistics.median(r[k] for r in rows)} for k in keys}
for case in hashes:
 for mode in ('default','checked'):
  before=result['timings'][case+'/baseline'];after=result['timings'][case+'/'+mode];pair={}
  for key in before.keys() & after.keys():
   a,b=before[key]['median'],after[key]['median'];delta=b-a;material=abs(delta)>100000 and abs(delta)/a>0.1
   pair[key]={'baseline_ns':a,'candidate_ns':b,'delta_ns':delta,'candidate_over_baseline':b/a if a else None,'material':material,'classification':('slower' if delta>0 else 'faster') if material else 'comparable'}
  result['comparisons'][case+'/'+mode]=pair
with (base/'summary.json').open('x') as f:json.dump(result,f,indent=2);f.write('\n')
print(json.dumps({'window_seconds':result['window_seconds'],'hashes':result['hashes'],'comparisons':result['comparisons']},indent=2))
