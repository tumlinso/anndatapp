#!/usr/bin/env python3
"""Validate retained receipts/identities and summarize without running workloads."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics


def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def distribution(values):
    return dict(median_ms=statistics.median(values)/1e6,min_ms=min(values)/1e6,max_ms=max(values)/1e6,samples_ms=[x/1e6 for x in values])


def main():
    p=argparse.ArgumentParser();p.add_argument('evidence',type=Path);a=p.parse_args();e=a.evidence.absolute()
    frozen=json.loads((e/'frozen-inputs.json').read_text())
    for path,item in frozen.items():
        if Path(path).stat().st_size!=item['bytes'] or sha(path)!=item['sha256']:raise RuntimeError('frozen identity differs: '+path)
    cases={};diagnostics={};windows={};parsed_records=[];expected_counts={}
    for window in ('screen','volume','reuse'):
        plan_path=e/(window+'-plan.json');plan=json.loads(plan_path.read_text())
        rows=[json.loads(x) for x in (e/(window+'-receipt.jsonl')).read_text().splitlines()]
        if rows[-1].get('status')!='completed_schedule' or rows[-1]['seconds']>30:raise RuntimeError('incomplete or overbudget window: '+window)
        if rows[0]['metadata']['plan_sha256']!=sha(plan_path):raise RuntimeError('plan identity differs')
        for path,digest in rows[0]['metadata']['identity_sha256'].items():
            if sha(path)!=digest:raise RuntimeError('receipt identity differs: '+path)
        expected={(c['name'],command['version']) for c in plan['cases'] for command in c['commands']}
        for item in plan['cases']:
            if item['name'].endswith('-diagnostic'):continue
            name=item['name'].split('@')[0].removesuffix('-checked')
            for command in item['commands']:
                key=(name,command['version']);expected_counts[key]=expected_counts.get(key,0)+1
        observed={(r.get('case'),r.get('version')) for r in rows[1:-1]}
        if expected!=observed or len(rows)-2!=len(expected):raise RuntimeError('schedule does not match plan')
        windows[window]=dict(seconds=rows[-1]['seconds'],processes=len(rows)-2,cpu=rows[0]['metadata']['cpu'],receipt_sha256=sha(e/(window+'-receipt.jsonl')),plan_sha256=sha(plan_path))
        for record in rows[1:-1]:
            if record.get('status')!='completed' or record.get('returncode')!=0:raise RuntimeError('failed process: '+record['case'])
            value=json.loads(record['stdout'])
            if value.get('golden')!='pass':raise RuntimeError('oracle did not pass')
            value['version']=record['version'];value['process_seconds']=record['process_seconds'];parsed_records.append(value)
            name=value['case']
            if record['case'].endswith('-diagnostic'):
                diagnostics[name]=value
            else:
                cases.setdefault(name,{}).setdefault(record['version'],[]).append(value)
    summaries={}
    for name,versions in cases.items():
        digests={row['output_fnv1a64'] for samples in versions.values() for row in samples}
        if len(digests)!=1:raise RuntimeError('logical output digest mismatch: '+name)
        result=dict(output_fnv1a64=next(iter(digests)),modes={})
        for version,samples in versions.items():
            if len(samples)!=expected_counts[(name,version)]:raise RuntimeError('process observation count differs from plan: '+name+'/'+version)
            mode=dict(observations=len(samples),end_to_end=distribution([x['end_to_end_ns'] for x in samples]),open=distribution([x['open_ns'] for x in samples]),nnz=samples[0]['nnz'],shape=samples[0]['shape'])
            for metric in ('selection_ns','preparation_requirements_ns','buffer_allocation_ns','transfer_ns','operation_ns'):
                if metric in samples[0]:mode[metric.removesuffix('_ns')]=distribution([x[metric] for x in samples])
            if samples[0].get('repeat_transfer_ns'):
                mode['reused_operation']=distribution([statistics.median(x['repeat_transfer_ns']) for x in samples])
                mode['reused_operation']['within_process_samples_ms']=[[y/1e6 for y in x['repeat_transfer_ns']] for x in samples]
            mode['native_dtypes']={key:samples[0][key] for key in ('value_dtype','actual_index_dtype','actual_offset_dtype') if key in samples[0]}
            result['modes'][version]=mode
        if 'default' in result['modes'] and 'python' in result['modes']:
            result['cpp_over_python_end_to_end']=result['modes']['default']['end_to_end']['median_ms']/result['modes']['python']['end_to_end']['median_ms']
            if 'reused_operation' in result['modes']['default']:
                result['cpp_over_python_reused_api_operation']=result['modes']['default']['reused_operation']['median_ms']/result['modes']['python']['reused_operation']['median_ms']
        summaries[name]=result
    inventory=json.loads((e/'fixtures-complete/inventory.json').read_text())
    generation=json.loads((e/'generation-state.json').read_text())
    fixture_bytes=sum(p.stat().st_size for directory in (e/'fixtures',e/'fixtures-complete') for p in directory.rglob('*') if p.is_file())
    if fixture_bytes!=generation['new_fixture_bytes_including_preserved_partial'] or fixture_bytes>128*1024*1024:raise RuntimeError('fixture budget or identity differs')
    output=dict(schema='anndatapp.collection-matrix-results.v1',source_commit='32852b6',qualified_io_archive_sha256=sha(e.parent/'optional-axis-validation-2026-11-17/candidate-prefix/lib/libanndatapp.a'),new_fixture_bytes=fixture_bytes,accepted_fixture_inventory_bytes=inventory['bytes'],frozen_files=len(frozen),windows=windows,cases=summaries,diagnostics=diagnostics,all_outputs_verified=True,records=parsed_records,limitations=['Eight primary cases have three independent observations per mode; five auxiliary cases have one observation. This is a bounded screen, no statistical tail qualification.','Normal caches and no eviction; fresh-process runs are not cold-storage measurements.','Logical selected CSR output equivalence is independently verified; canonical per-row ordering is outside clocks, native sparse ordering and widths may differ.','C++ default retains full structural/sparse checks but skips cross-component axis-name comparisons; checked mode is auxiliary.','Repeated C++ reads reuse a prepared plan and caller buffers; Python repeats its native same-handle X indexing including allocation. These are API/lifecycle comparisons, not equal-work kernels.','Timing counters are separate diagnostics, exclude all Python/Rust allocators and physical device I/O; no zero-allocation claim.','CSR float32, int32 indices/int64 stored offsets; no dense/CSC, cold-cache or large-scale throughput qualification.'])
    path=e/'summary.json'
    if path.exists():raise RuntimeError('summary exists; preserve it')
    path.write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(dict(windows=windows,cases={name:dict(cpp_ms=x['modes'].get('default',{}).get('end_to_end',{}).get('median_ms'),python_ms=x['modes'].get('python',{}).get('end_to_end',{}).get('median_ms'),ratio=x.get('cpp_over_python_end_to_end')) for name,x in summaries.items()}),indent=2))

if __name__=='__main__':main()
