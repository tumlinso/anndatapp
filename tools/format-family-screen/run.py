#!/usr/bin/env python3
"""Run paired screen serially under one strict 30-second process budget."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--baseline', required=True, type=Path)
    p.add_argument('--candidate', required=True, type=Path)
    p.add_argument('--fixtures', required=True, type=Path)
    p.add_argument('--baseline-prefix', required=True, type=Path)
    p.add_argument('--candidate-prefix', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--case-timeout', type=float, default=3.0)
    p.add_argument('--samples', type=int, choices=(1, 2, 3), default=1)
    a = p.parse_args()
    if not (0 < a.case_timeout <= 5):
        p.error('case timeout must be in (0, 5] seconds')
    if a.output.exists():
        p.error('output exists; preserve previous results and choose a new path')
    for path in (a.baseline, a.candidate, a.fixtures / 'inventory.json',
                 a.baseline_prefix / 'lib/libanndatapp.a', a.candidate_prefix / 'lib/libanndatapp.a'):
        if not path.is_file():
            p.error(f'missing {path}')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    # Record provenance before the timing window, including both exact binaries.
    sources = sorted(Path(__file__).resolve().parent.glob('*'))
    compiler = subprocess.run(['clang++-18', '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    metadata = {'compiler': compiler, 'screen_source_sha256': {p.name: digest(p) for p in sources if p.is_file()},
                'baseline_library_sha256': digest(a.baseline_prefix / 'lib/libanndatapp.a'),
                'candidate_library_sha256': digest(a.candidate_prefix / 'lib/libanndatapp.a'),
                'baseline_binary': str(a.baseline.resolve()), 'baseline_sha256': digest(a.baseline),
                'candidate_binary': str(a.candidate.resolve()), 'candidate_sha256': digest(a.candidate),
                'fixture_inventory_sha256': digest(a.fixtures / 'inventory.json'),
                'platform': platform.platform(), 'cache': 'normal OS/HDF5 caches, no eviction',
                'max_window_seconds': 30, 'case_timeout_seconds': a.case_timeout,
                'samples_requested': a.samples, 'order': 'paired serial; alternating first version',
                'measurement_limits': 'no interposer; physical I/O, allocations and retained scratch unmeasured'}
    cases = ('open', 'mono-open', 'map', 'read', 'mono-read', 'inspect', 'view')
    records = []
    started = time.monotonic()
    deadline = started + 30.0
    with a.output.open('x', buffering=1) as output:
        output.write(json.dumps({'metadata': metadata})+'\n')
        for sample in range(a.samples):
            for size in (100000, 10000):
                directory = a.fixtures / str(size)
                for index, case in enumerate(cases):
                    versions = [('baseline', a.baseline), ('candidate', a.candidate)]
                    if (sample + index) % 2:
                        versions.reverse()
                    for version, binary in versions:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0.05:
                            output.write(json.dumps({'status': 'budget_exhausted', 'seconds': time.monotonic()-started})+'\n')
                            return
                        component = directory / ('monolithic.h5ad' if case.startswith('mono-') else 'part-0.h5ad')
                        manifest = '-' if case == 'map' or case.startswith('mono-') else str(directory / 'collection.h5ads')
                        cmd = [str(binary.resolve()), case, manifest, str(component)]
                        before = time.monotonic()
                        try:
                            result = subprocess.run(cmd, capture_output=True, text=True,
                                                    timeout=min(a.case_timeout, remaining), check=False)
                            record = {'status': 'completed' if result.returncode == 0 else 'failed',
                                      'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
                        except subprocess.TimeoutExpired as e:
                            record = {'status': 'timeout', 'stdout': (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or ''),
                                      'stderr': (e.stderr or b'').decode(errors='replace') if isinstance(e.stderr, bytes) else (e.stderr or '')}
                        record.update({'sample': sample+1, 'size': size, 'case': case, 'version': version,
                                       'process_seconds': time.monotonic()-before})
                        output.write(json.dumps(record)+'\n')
                        records.append(record)
        output.write(json.dumps({'status': 'completed_schedule', 'seconds': time.monotonic()-started})+'\n')

if __name__ == '__main__':
    main()
