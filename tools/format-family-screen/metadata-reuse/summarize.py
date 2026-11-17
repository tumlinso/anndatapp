#!/usr/bin/env python3
"""Verify a frozen collection timing receipt and preserve descriptive statistics."""
import argparse,hashlib,json,statistics
from collections import defaultdict
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);d=p.parse_args().evidence.absolute()
plan=json.loads((d/'plan.json').read_text()); lines=[json.loads(x) for x in (d/'receipt.jsonl').read_text().splitlines()]
meta=lines[0]['metadata']; end=lines[-1]
if end.get('status')!='completed_schedule' or end['seconds']>=30: raise RuntimeError('incomplete or over-budget')
for path,expected in meta['identity_sha256'].items():
 with open(path,'rb') as f: actual=hashlib.file_digest(f,'sha256').hexdigest()
 if actual!=expected: raise RuntimeError('identity changed '+path)
expected={(x['name'],c['version']) for x in plan['cases'] for c in x['commands']}; records=[r for r in lines if 'version' in r]
if len(records)!=22 or {(r['case'],r['version']) for r in records}!=expected: raise RuntimeError('schedule differs')
values=defaultdict(list); hashes=defaultdict(set); diags={}; native={}
for r in records:
 if r['status']!='completed': raise RuntimeError('failed process: '+str(r))
 o=json.loads(r['stdout']); case=r['case'].split('@')[0].removesuffix('-diagnostic'); out=o.get('output',o)
 if o.get('golden')!='pass':raise RuntimeError('oracle failed')
 if out['shape']!=[64,64] or out['nnz']!=({'dataset-small':512,'dataset-10k':17}[case]) or out['value_dtype']!='float32': raise RuntimeError('shape/dtype mismatch')
 idx=out.get('actual_index_dtype',out.get('index_dtype')); off=out.get('actual_offset_dtype',out.get('offset_dtype'))
 if r['version']=='python':
  nd=out['native_dtypes']; idx,off=nd['indices'],nd['indptr']
 if idx!='int32' or off!=('int64' if case=='dataset-10k' and r['version']!='python' else 'int32'): raise RuntimeError('native index dtype mismatch')
 native[case+'/'+r['version']]={'indices':idx,'indptr':off}
 hashes[case].add(out['output_fnv1a64'])
 if r['case'].endswith('-diagnostic'):diags[case+'/'+r['version']]=o['diagnostic']
 else:values[case+'/'+r['version']].append(o)
if any(len(x)!=1 for x in hashes.values()):raise RuntimeError('output hash mismatch')
timings={}
for key,rows in values.items():
 if len(rows)!=3: raise RuntimeError('bad sample count')
 fields=[x for x in ('open_ns','selection_ns','slice_ns','preparation_requirements_ns','prepare_requirements_ns','buffer_allocation_ns','output_allocation_ns','transfer_ns','read_into_ns','operation_ns','end_to_end_ns') if x in rows[0]]
 timings[key]={x:{'samples':[r[x] for r in rows],'median':statistics.median(r[x] for r in rows)} for x in fields}
result={'window_seconds':end['seconds'],'record_count':len(records),'metadata':meta,'output_fnv1a64':{k:next(iter(v)) for k,v in hashes.items()},'native_dtypes':native,'timings':timings,'diagnostics':diags,'caveats':['Three fresh process samples per case; normal OS/HDF5 caches. Independent medians need not add.','Python and C++ use different library and validation work. Selector construction is outside measured small clocks for both; 10k follows original separate clock boundary.','Diagnostic counters are interposed observations, not physical I/O or live memory; no Python interposition.']}
with (d/'summary.json').open('x') as f: json.dump(result,f,indent=2);f.write('\n')
print(json.dumps({'window_seconds':end['seconds'],'hashes':result['output_fnv1a64'],'timings':timings},indent=2))
