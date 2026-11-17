#!/usr/bin/env python3
"""Analyze the sealed campaign, allowing only its exact Python stride failure.

The original strict summarizer remains frozen. This post-campaign adapter reuses
its digest/statistics helpers and preserves the incorrect-output record rather
than treating its latency as a successful comparison.
"""
import argparse
import json
from pathlib import Path
import statistics
from summarize import sha, distribution


def known_stride_failure(record):
    return (record.get('case') == 'positive-stride@1' and record.get('version') == 'python'
            and record.get('status') == 'failed' and record.get('returncode') == 1
            and record.get('stdout') == ''
            and 'ValueError: expected CSR shape (64, 64), got csr (8065, 63)' in record.get('stderr', ''))


def main():
    p=argparse.ArgumentParser();p.add_argument('evidence',type=Path);a=p.parse_args();e=a.evidence.absolute()
    frozen=json.loads((e/'frozen-inputs.json').read_text())
    for path,item in frozen.items():
        if Path(path).stat().st_size != item['bytes'] or sha(path) != item['sha256']:
            raise RuntimeError('frozen identity differs: '+path)
    cases={};diagnostics={};windows={};expected_counts={};failures=[];records=[]
    for window in ('screen','volume','reuse'):
        plan_path=e/(window+'-plan.json');plan=json.loads(plan_path.read_text())
        receipt=e/(window+'-receipt.jsonl');rows=[json.loads(x) for x in receipt.read_text().splitlines()]
        header=rows[0]['metadata'];footer=rows[-1]
        if footer.get('status') != 'completed_schedule' or footer['seconds'] > 30:
            raise RuntimeError('incomplete or overbudget schedule: '+window)
        if header['plan_sha256'] != sha(plan_path):raise RuntimeError('plan differs')
        for path,digest in header['identity_sha256'].items():
            if sha(path) != digest:raise RuntimeError('receipt identity differs: '+path)
        expected={(item['name'],cmd['version']):cmd for item in plan['cases'] for cmd in item['commands']}
        observed={(row.get('case'),row.get('version')) for row in rows[1:-1]}
        if set(expected) != observed or len(rows)-2 != len(expected):raise RuntimeError('schedule differs')
        windows[window]=dict(seconds=footer['seconds'],processes=len(rows)-2,cpu=header['cpu'],receipt_sha256=sha(receipt),plan_sha256=sha(plan_path))
        for row in rows[1:-1]:
            command=expected[(row['case'],row['version'])]
            if row['argv'] != command['argv'] or row['env_overrides'] != command.get('env',{}):
                raise RuntimeError('executed command differs from plan')
            if known_stride_failure(row):
                failures.append(dict(window=window,case='positive-stride',version='python',status='incorrect_output',expected_shape=[64,64],observed_shape=[8065,63],latency_comparison='excluded',raw_record=row))
                continue
            if row.get('status') != 'completed' or row.get('returncode') != 0:
                raise RuntimeError('unexpected failure: '+str(row))
            value=json.loads(row['stdout'])
            if value.get('golden') != 'pass':raise RuntimeError('oracle did not pass')
            name=value['case'];version=row['version']
            if row['case'].endswith('-diagnostic'):
                diagnostics[name]=value
            else:
                key=(name,version);expected_counts[key]=expected_counts.get(key,0)+1
                cases.setdefault(name,{}).setdefault(version,[]).append(value)
            records.append(dict(window=window,scheduled_case=row['case'],version=version,process_seconds=row['process_seconds'],result=value))
    if len(failures) != 1:raise RuntimeError('expected exactly the recorded Python stride failure')
    summaries={}
    for name,versions in cases.items():
        digests={row['output_fnv1a64'] for samples in versions.values() for row in samples}
        if len(digests) != 1:raise RuntimeError('canonical output mismatch: '+name)
        result=dict(output_fnv1a64=next(iter(digests)),modes={})
        for version,samples in versions.items():
            mode=dict(observations=len(samples),end_to_end=distribution([x['end_to_end_ns'] for x in samples]),open=distribution([x['open_ns'] for x in samples]),nnz=samples[0]['nnz'],shape=samples[0]['shape'])
            for metric in ('selection_ns','preparation_requirements_ns','buffer_allocation_ns','transfer_ns','operation_ns'):
                if metric in samples[0]:mode[metric.removesuffix('_ns')]=distribution([x[metric] for x in samples])
            if samples[0].get('repeat_transfer_ns'):
                mode['reused_operation']=distribution([statistics.median(x['repeat_transfer_ns']) for x in samples])
                mode['reused_operation']['within_process_samples_ms']=[[y/1e6 for y in x['repeat_transfer_ns']] for x in samples]
            mode['native_dtypes']={key:samples[0][key] for key in ('value_dtype','actual_index_dtype','actual_offset_dtype')}
            mode['selector_kinds']={key:samples[0][key] for key in ('row_selector_kind','col_selector_kind')}
            result['modes'][version]=mode
        if 'default' in versions and 'python' in versions:
            result['comparison_status']='verified_equivalent_logical_output'
            result['cpp_over_python_end_to_end']=result['modes']['default']['end_to_end']['median_ms']/result['modes']['python']['end_to_end']['median_ms']
            if 'reused_operation' in result['modes']['default']:
                result['cpp_over_python_reused_api_operation']=result['modes']['default']['reused_operation']['median_ms']/result['modes']['python']['reused_operation']['median_ms']
        elif name == 'positive-stride':result['comparison_status']='python_incorrect_output_no_latency_ratio'
        else:raise RuntimeError('missing comparison mode: '+name)
        summaries[name]=result
    generation=json.loads((e/'generation-state.json').read_text())
    fixture_bytes=sum(p.stat().st_size for directory in (e/'fixtures',e/'fixtures-complete') for p in directory.rglob('*') if p.is_file())
    if fixture_bytes != generation['new_fixture_bytes_including_preserved_partial'] or fixture_bytes > 128*1024*1024:
        raise RuntimeError('fixture budget or physical inventory differs')
    out=dict(schema='anndatapp.collection-matrix-results.v2',source_commit='32852b6',analyzer_sha256=sha(__file__),frozen_files=len(frozen),new_fixture_bytes=fixture_bytes,windows=windows,cases=summaries,diagnostics=diagnostics,capability_failures=failures,successful_records=len(records),all_successful_outputs_verified=True,records=records,
             limitations=['Eight primary cases have three independent process observations per mode; auxiliary controls have one.','Normal OS/HDF5 caches; hashing may warm stores. Fresh process does not mean cold storage.','Logical selected CSR equivalence; canonical ordering is outside clocks, native sparse ordering/widths may differ.','Repeated C++ reads reuse prepared plans/caller buffers; Python repeats native same-handle indexing/allocation. API lifecycle comparison, not equal allocation-free kernels.','C++ diagnostics are separate intercepted calls/new requests, not physical traffic, all allocators or live memory. Reuse itself was not instrumented.','Float32 CSR only; no dense/CSC, cold-cache or larger-than-memory throughput qualification.','The Python positive-stride result was incorrect; its latency is excluded.'])
    path=e/'summary.json'
    if path.exists():raise RuntimeError('summary exists; preserve it')
    path.write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps(dict(windows=windows,successful_records=len(records),capability_failures=len(failures),cases={name:dict(cpp_ms=x['modes']['default']['end_to_end']['median_ms'],python_ms=x['modes'].get('python',{}).get('end_to_end',{}).get('median_ms'),ratio=x.get('cpp_over_python_end_to_end'),observations=x['modes']['default']['observations']) for name,x in summaries.items()}),indent=2))

if __name__=='__main__':main()
