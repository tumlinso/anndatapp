#!/usr/bin/env python3
"""Check completed receipts and summarize; never launch another measurement."""
import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
from pathlib import Path
from statistics import median


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('evidence', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    base = a.evidence.absolute()
    if a.output.exists():
        raise FileExistsError(a.output)
    identities = json.loads((base/'input-identities.json').read_text())
    for name, entry in identities.items():
        file = Path(name)
        if file.stat().st_size != entry['bytes'] or sha(file) != entry['sha256']:
            raise RuntimeError(f'input changed: {name}')
    timings = defaultdict(list)
    diagnostics = {}
    hashes = defaultdict(set)
    receipts, windows = {}, {}
    records = 0
    for window in ('raster', 'collection', 'diagnostic'):
        path = base/f'{window}-results.jsonl'
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        if rows[-1].get('status') != 'completed_schedule' or rows[-1]['seconds'] >= 30:
            raise RuntimeError(f'incomplete or over-budget {window} window')
        windows[window] = {'seconds': rows[-1]['seconds'], 'process_records': 0,
            'settings': {k: rows[0]['metadata'][k] for k in
                ('cpu', 'thread_limits', 'plan_sha256', 'runner_sha256', 'platform',
                 'window_seconds', 'case_timeout_seconds', 'cache', 'execution')}}
        for row in rows:
            if 'version' not in row:
                continue
            records += 1
            windows[window]['process_records'] += 1
            if row['status'] != 'completed':
                raise RuntimeError(f'failed record: {row["case"]} {row["version"]}')
            result = json.loads(row['stdout'])
            if result.get('golden') != 'pass':
                raise RuntimeError(f'failed golden: {row["case"]}')
            case = row['case'].split('@')[0]
            output = result.get('output', result)
            hashes[case.removesuffix('-raw')].add(output['output_fnv1a64'])
            if window != 'diagnostic':
                timings[(case, row['version'])].append(result)
                continue
            phases = result.get('diagnostic', {}).get('phases') or result['diagnostic_phases']
            chunk_reads = defaultdict(lambda: {'read_calls': 0, 'read_bytes': 0})
            for line in row['stderr'].splitlines():
                if not line.startswith('release11_chunk_reads: '):
                    continue
                observed = json.loads(line.split(': ', 1)[1])
                if observed['omitted_paths']:
                    raise RuntimeError('observer omitted chunk paths')
                for chunk in observed['chunks']:
                    counts = chunk_reads[chunk['path']]
                    counts['read_calls'] += chunk['calls']
                    counts['read_bytes'] += chunk['bytes']
            if case.startswith('raster-'):
                expected = {'/images/image/s0/c/0/0/0'}
                if 'multichunk' in case:
                    expected = {f'/images/image/s0/c/0/{y}/{x}' for y in (0, 1) for x in (0, 1)}
                if set(chunk_reads) != expected or any(x['read_calls'] != 1 for x in chunk_reads.values()):
                    raise RuntimeError(f'non-minimal touched-chunk reads: {case} {row["version"]}')
            def io(phase):
                return (phase['v1_io_slots'][:4] if 'v1_io_slots' in phase else
                        [phase['intercepted_read_calls'], phase['intercepted_read_bytes'],
                         phase['intercepted_write_calls'], phase['intercepted_write_bytes']])
            phase_counts = {k: {'read_calls': io(v)[0], 'read_bytes': io(v)[1],
                                'write_calls': io(v)[2], 'write_bytes': io(v)[3],
                                'observed_cpp_new_calls': v['cpp_new_calls'],
                                'requested_cpp_new_bytes': v['cpp_new_bytes']} for k, v in phases.items()}
            diagnostics[f'{case}/{row["version"]}'] = {'phases': phase_counts,
                'read_calls': sum(v['read_calls'] for v in phase_counts.values()),
                'read_bytes': sum(v['read_bytes'] for v in phase_counts.values()),
                'chunk_reads': dict(chunk_reads),
                'cpp_new_scope': 'Observed C++ operator new only; excludes Python/NumPy/Rust allocators. Not a cross-language allocation comparison.'}
        for suffix in ('plan.json', 'results.jsonl'):
            file = base/f'{window}-{suffix}'
            receipts[file.name] = sha(file)
    if records != 60 or any(len(x) != 1 for x in hashes.values()):
        raise RuntimeError('incomplete records or cross-language output mismatch')
    groups = {}
    for (case, version), rows in timings.items():
        fields = [k for k in rows[0] if k.endswith('_ns') and isinstance(rows[0][k], int)]
        groups[f'{case}/{version}'] = {'samples': len(rows),
            'median_ns': {k: median(r[k] for r in rows) for k in fields},
            'samples_ns': [{k: r[k] for k in fields} for r in rows],
            'output_metadata': rows[0].get('output', {k: rows[0][k] for k in
                ('shape', 'nnz', 'value_dtype', 'actual_index_dtype', 'actual_offset_dtype', 'dtype') if k in rows[0]})}
    comparisons = {}
    for case in sorted({x[0] for x in timings}):
        cpp = groups[f'{case}/cpp']['median_ns']['end_to_end_ns']
        for version in ('python', 'python-selected-axis-checks'):
            if f'{case}/{version}' not in groups:
                continue
            py = groups[f'{case}/{version}']['median_ns']['end_to_end_ns']
            delta = cpp-py
            material = abs(delta) > 100000 and abs(delta)/py > 0.1
            comparisons[f'{case}/{version}'] = {'cpp_over_python': cpp/py,
                'cpp_minus_python_ns': delta, 'material_by_screen_threshold': material,
                'classification': ('slower' if delta > 0 else 'faster') if material else 'comparable',
                'timing_evidence': 'one paired observation only' if '-raw' in case else 'three fresh-process paired observations'}
    state = json.loads((base/'campaign-state.json').read_text())
    started = min(datetime.fromisoformat(x['started_utc']) for x in state['windows'].values())
    finished = max(datetime.fromisoformat(x['finished_utc']) for x in state['windows'].values())
    result = {'schema': 'anndatapp.release11-python-performance.v1',
        'library_commit': '180c440ad4115306ede27bc2e86f927bee4acf94',
        'scope': 'performance comparison only; release decision remains with user',
        'windows': windows, 'total_window_seconds': sum(x['seconds'] for x in windows.values()),
        'first_start_to_final_finish_seconds': (finished-started).total_seconds(),
        'process_records': records, 'failures': 0,
        'output_fnv1a64': {k: next(iter(v)) for k, v in hashes.items()},
        'timings': groups, 'comparisons': comparisons, 'diagnostics': diagnostics,
        'raw_receipts_sha256': receipts, 'input_identities_sha256': sha(base/'input-identities.json'),
        'evidence_root': str(base),
        'limits': ['Normal caches; hashing and prior work may warm them. No cold-cache or scaling claim.',
                   'First-call public API includes lazy setup after imports. Raw controls bypass domain metadata.',
                   'Python axis-check adapter reopens HDF5 resources and is auxiliary, not the upstream native API or full C++ validation contract.',
                   'C++ 10k offsets are int64; Python returns int32. Canonical output digest conversion is outside timing.',
                   'Intercepted libc calls/bytes are not physical-device I/O. HDF5 logical calls and decoder frame counts are unavailable in this observer.',
                   'One/four chunk reads prove observed fetch selectivity, not a universal decode-count or memory bound.',
                   'Allocation counts omit other allocators and are cumulative requests, not live memory.',
                   'No production changes, dependency upgrades, broader qualification or publication.']}
    with a.output.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(json.dumps({'records': records, 'seconds': result['total_window_seconds'],
                      'output': str(a.output), 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
