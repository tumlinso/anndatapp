#!/usr/bin/env python3
"""Run one granted, bounded v1 pass; inputs and output must be fresh."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
SCREEN = ROOT / 'build/format-family-io/screen/core-inputs'
BUILDS = {
    'pre': ROOT / 'build/format-family-io/core-screen-pre',
    'bbe': ROOT / 'build/format-family-io/core-screen-bbe-off',
    'candidate': ROOT / 'build/format-family-io/core-screen-candidate-off',
}
PREFIXES = {
    'pre': ROOT / 'build/format-family-io/pre-extension-prefix',
    'bbe': ROOT / 'build/format-family-io/before-core-prefix',
    'candidate': ROOT / 'build/format-family-io/candidate-core-prefix',
}
MANIFEST_HASH = 'd29c7557a6957e1536628cbaa824f26702fd5c67aa6e0c016d7058bf6345d754'
SCENARIOS = ('h5ad', 'zarr2', 'zarr3', 'contexts')
VERSIONS = ('pre', 'bbe', 'candidate')
MMAP = ROOT / 'build/format-family-io/screen/mmap-fixture'
H2H = Path('/home/tumlinson/anndatapp/build/evidence/head-to-head-baseline-20261104/head-to-head/fixtures')
H2H_CASES = {
    'h5ad_eager_read_dense': ('dense.h5ad', 'dense_x_sha256'),
    'h5ad_sparse_rows_csr': ('csr.h5ad', 'sparse_csr_rows'),
    'h5ad_sparse_cols_csc': ('csc.h5ad', 'sparse_csc_columns'),
    'zarr3_gather_read': ('sharded.zarr', 'gather'),
}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def verify_inputs(path):
    manifest = path / 'manifest.json'
    if digest(manifest) != MANIFEST_HASH:
        raise ValueError(f'fixture manifest changed: {manifest}')
    data = json.loads(manifest.read_text())
    files = 0
    for name, info in data['fixtures'].items():
        item = path / name
        for rel, record in info['files'].items():
            file = item / rel if item.is_dir() else item
            if file.stat().st_size != record['bytes'] or digest(file) != record['sha256']:
                raise ValueError(f'fixture changed: {file}')
            files += 1
    return {'manifest_sha256': MANIFEST_HASH, 'verified_files': files}


def dependencies(binary):
    out = subprocess.run(['ldd', str(binary)], check=True, capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        if 'not found' in line:
            raise RuntimeError(f'missing shared library: {line}')
        part = line.split('=>', 1)[-1].strip().split(' ', 1)[0]
        p = Path(part)
        if p.is_absolute() and p.is_file():
            found[str(p.resolve())] = digest(p)
    return found


def verify_h2h():
    manifest = H2H / 'fixture-manifest.json'
    expected_file = H2H / '1024/expected.json'
    if digest(manifest) != 'ece7b87fd5e62290da83e668098400d2ac16046cc2a1bd40b051ec2e73a21e40':
        raise ValueError('historical head-to-head manifest changed')
    if digest(expected_file) != '94dee879d32d02bb807a112591b7bd2d008dcf17b9854956d1e32c0b5ce3bfcb':
        raise ValueError('historical head-to-head expected data changed')
    m = json.loads(manifest.read_text())['fixtures']['1024']
    cases = [(H2H / '1024' / (name + '.h5ad'), info) for name, info in m['h5ad'].items()]
    cases.append((H2H / '1024/sharded.zarr', m['zarr3']))
    n = 0
    for base, info in cases:
        for rel, record in info['files'].items():
            file = base / rel if base.is_dir() else base
            if file.stat().st_size != record['bytes'] or digest(file) != record['sha256']:
                raise ValueError(f'head-to-head fixture changed: {file}')
            n += 1
    return json.loads(expected_file.read_text()), {'manifest_sha256': digest(manifest),
                                                    'expected_sha256': digest(expected_file), 'verified_files': n}


def write_json(path, obj):
    with path.open('x') as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('timing', 'diagnostic'))
    parser.add_argument('--output', required=True, type=Path, help='new output directory')
    args = parser.parse_args()
    out = args.output.resolve()
    # Ensure that every prerequisite is ready before creating the output or starting the window.
    provenance = {'phase': args.phase, 'window_seconds': 30, 'case_timeout_seconds': 5,
                  'sources': {}, 'builds': {}, 'fixtures': {}}
    provenance['prefixes_ready_sha256'] = digest(ROOT / 'build/format-family-io/prefixes-ready.json')
    for rel in ('tools/format-family-screen/core_window.py', 'tools/format-family-screen/core_CMakeLists.txt',
                'benchmarks/v1/main.cpp', 'benchmarks/v1/core_cases.cpp', 'benchmarks/v1/storage_cases.cpp',
                'benchmarks/v1/harness.hpp', 'benchmarks/v1/io_counters.c', 'benchmarks/v1/alloc_counters.cpp',
                'benchmarks/v1/make_fixtures.py', 'benchmarks/head_to_head/cpp_driver.cpp',
                'benchmarks/head_to_head/make_fixture.py'):
        provenance['sources'][rel] = digest(ROOT / rel)
    for version, build in BUILDS.items():
        binary = build / 'core_v1'
        mmap_binary = build / 'core_mmap'
        libs = [build / 'libcore_v1_io.so', build / 'libcore_v1_alloc.so']
        cache = (build / 'CMakeCache.txt').read_text()
        prefix_entries = [line.split('=', 1)[1] for line in cache.splitlines()
                          if line.startswith('CMAKE_PREFIX_PATH:')]
        if len(prefix_entries) != 1 or Path(prefix_entries[0].split(';')[0]).resolve() != PREFIXES[version].resolve():
            raise ValueError(f'{version} consumer was not configured against the required prefix')
        prefix_libraries = sorted((PREFIXES[version] / 'lib').glob('libanndatapp*'))
        if not prefix_libraries:
            raise ValueError(f'{version} prefix contains no anndatapp library')
        provenance['builds'][version] = {'binary': str(binary), 'binary_sha256': digest(binary),
                                          'prefix': str(PREFIXES[version]),
                                          'prefix_libraries': {str(p): digest(p) for p in prefix_libraries if p.is_file()},
                                          'mmap_binary_sha256': digest(mmap_binary),
                                          'head_to_head_binary_sha256': digest(build / 'core_head_to_head'),
                                          'shared_dependencies': dependencies(binary),
                                          'mmap_shared_dependencies': dependencies(mmap_binary),
                                          'observer_sha256': {str(p): digest(p) for p in libs},
                                          'cmake_cache_sha256': digest(build / 'CMakeCache.txt')}
        provenance['fixtures'][version] = verify_inputs(SCREEN / args.phase / version)
    expected = json.loads((MMAP / 'expected.json').read_text())
    mmap_file = MMAP / expected['file']
    if digest(mmap_file) != expected['file_sha256'] or mmap_file.stat().st_size != expected['file_bytes']:
        raise ValueError('mmap fixture differs from independent manifest')
    provenance['mmap_fixture'] = {'file_sha256': expected['file_sha256'],
                                   'expected_sha256': digest(MMAP / 'expected.json'),
                                   'payload_sha256': expected['payload_sha256']}
    h2h_expected, provenance['head_to_head_fixture'] = verify_h2h()
    provenance['head_to_head_fixture']['historical_generator_sha256'] = 'fcd5b58df8939167981ce6070280eed3341bcd067c6ff8958bd09bc211475207'
    original_affinity = sorted(os.sched_getaffinity(0))
    if not original_affinity: raise RuntimeError('empty allowed CPU affinity')
    selected_cpu = original_affinity[0]
    os.sched_setaffinity(0, {selected_cpu})
    provenance['cpu'] = selected_cpu
    provenance['original_affinity'] = original_affinity
    provenance['cache'] = 'normal OS/HDF5 caches; fixture hash preflight reads each input before window; no eviction'
    provenance['thread_limits'] = {k:'1' for k in ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'MKL_NUM_THREADS', 'NUMEXPR_NUM_THREADS', 'NUMBA_NUM_THREADS', 'BLOSC_NTHREADS')}
    out.mkdir(parents=True, exist_ok=False)
    write_json(out / 'provenance.json', provenance)
    records = []
    begin = time.monotonic()
    deadline = begin + 30
    scenarios = SCENARIOS + (('mmap',) + tuple(H2H_CASES) if args.phase == 'timing' else ())
    for i, scenario in enumerate(scenarios):
        # Rotate first version by scenario to avoid fixing order effects.
        order = VERSIONS[i % len(VERSIONS):] + VERSIONS[:i % len(VERSIONS)]
        for version in order:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                write_json(out / 'result.json', {'status': 'timeout', 'records': records})
                return 1
            build = BUILDS[version]
            fixture = SCREEN / args.phase / version
            cmd = ([str(build / 'core_mmap'), str(mmap_file)] if scenario == 'mmap'
                   else [str(build / 'core_v1'), scenario, str(fixture)])
            if scenario in H2H_CASES:
                cmd = [str(build / 'core_head_to_head'), scenario,
                       str(H2H / '1024' / H2H_CASES[scenario][0]), str(out / 'unused-output')]
            env = os.environ.copy()
            for key in ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'MKL_NUM_THREADS', 'NUMEXPR_NUM_THREADS', 'NUMBA_NUM_THREADS', 'BLOSC_NTHREADS'):
                env[key] = '1'
            env['PYTHONHASHSEED'] = '0'
            env.pop('LD_PRELOAD', None)
            if args.phase == 'diagnostic':
                env['LD_PRELOAD'] = f'{build / "libcore_v1_io.so"}:{build / "libcore_v1_alloc.so"}'
                cmd.append('--diagnostic')
            record = {'scenario': scenario, 'version': version, 'command': cmd,
                      'fixture': str(fixture), 'start_offset_seconds': time.monotonic() - begin}
            try:
                p = subprocess.run(cmd, input='continue\n' if scenario in H2H_CASES else None,
                                   capture_output=True, text=True, env=env, timeout=min(5.0, remaining))
                stdout, stderr, code = p.stdout, p.stderr, p.returncode
            except subprocess.TimeoutExpired as e:
                stdout = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
                stderr = (e.stderr or b'').decode(errors='replace') if isinstance(e.stderr, bytes) else (e.stderr or '')
                code = None
                record['error'] = ('global 30-second window expired' if remaining <= 5.0
                                   else 'case exceeded 5-second deadline')
            record.update(returncode=code, end_offset_seconds=time.monotonic() - begin,
                          stdout=stdout, stderr=stderr)
            try:
                lines = [json.loads(line) for line in stdout.splitlines() if line.strip()]
                if scenario == 'mmap':
                    valid = (len(lines) == 1 and lines[0].get('sha256') == expected['payload_sha256']
                             and lines[0].get('bytes') == expected['payload_bytes'])
                elif scenario in H2H_CASES:
                    golden = h2h_expected[H2H_CASES[scenario][1]]
                    golden = golden['sha256'] if isinstance(golden, dict) else golden
                    valid = (len(lines) == 2 and lines[0].get('phase') == 'measurement'
                             and lines[1].get('phase') == 'verification' and lines[1].get('sha256') == golden)
                    record['independent_expected_sha256'] = golden
                    record['phase_note'] = 'prepare_seconds combines open, selection, preparation and allocation'
                else:
                    valid = (bool(lines) and lines[-1].get('process_summary') and all(
                        x.get('validated', False) for x in lines[:-1]))
                if code != 0 or not valid:
                    record['error'] = record.get('error', 'invalid result or failed validation')
            except (ValueError, AttributeError):
                record['error'] = record.get('error', 'malformed result JSON')
            records.append(record)
            write_json(out / f'{len(records):02d}-{scenario}-{version}.json', record)
            if 'error' in record and record['error'] != 'case exceeded 5-second deadline':
                write_json(out / 'result.json', {'status': 'failed', 'records': records})
                return 1
    status = 'partial' if any('error' in r for r in records) else 'complete'
    write_json(out / 'result.json', {'status': status, 'window_elapsed_seconds': time.monotonic()-begin,
                                     'records': records})
    return 0 if status == 'complete' else 1


if __name__ == '__main__':
    sys.exit(main())
