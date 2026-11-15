#!/usr/bin/env python3
"""Execute a frozen JSON plan, serially, in one <=30 s measurement window."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--plan', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--samples', type=int, default=1, choices=(1,2,3))
    p.add_argument('--case-timeout', type=float, default=5)
    a = p.parse_args()
    if not 0 < a.case_timeout <= 10:
        p.error('case timeout must be between 0 and 10 seconds')
    if a.output.exists():
        p.error('output exists; preserve evidence and choose a new path')
    plan = json.loads(a.plan.read_text())
    if not plan.get('cases') or not plan.get('identity_files'):
        p.error('plan must specify cases and identity_files')
    for case in plan['cases']:
        if not case.get('name') or not case.get('commands'):
            p.error('every case requires a name and version/command list')
        for cmd in case['commands']:
            if not cmd.get('version') or not isinstance(cmd.get('argv'), list) or not cmd['argv']:
                p.error('every command needs version and argv')
    identities = {str(Path(p).resolve()): sha(p) for p in plan['identity_files']}
    a.output.parent.mkdir(parents=True, exist_ok=True)
    original_affinity = sorted(os.sched_getaffinity(0))
    if not original_affinity: raise RuntimeError('empty allowed CPU affinity')
    selected_cpu = original_affinity[0]
    os.sched_setaffinity(0, {selected_cpu})
    header = {'cpu': selected_cpu, 'original_affinity': original_affinity,
              'thread_limits': {'OMP_NUM_THREADS':'1', 'OPENBLAS_NUM_THREADS':'1', 'MKL_NUM_THREADS':'1', 'NUMEXPR_NUM_THREADS':'1', 'NUMBA_NUM_THREADS':'1', 'BLOSC_NTHREADS':'1'},
              'plan_sha256':sha(a.plan), 'identity_sha256':identities,
              'runner_sha256':sha(__file__), 'platform':platform.platform(),
              'window_seconds':30, 'case_timeout_seconds':a.case_timeout,
              'samples_requested':a.samples, 'cache':'normal OS/HDF5 caches, no eviction',
              'execution':'serial; version order reversed on alternate cases/samples',
              'scope':'separate uninstrumented timing and interposed diagnostic plans; counters are not total OS I/O'}
    base_env = os.environ.copy()
    base_env.update({k:'1' for k in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS','NUMEXPR_NUM_THREADS','NUMBA_NUM_THREADS','BLOSC_NTHREADS')})
    base_env['PYTHONHASHSEED']='0'
    base_env.pop('LD_PRELOAD',None)
    start = time.monotonic()
    deadline = start+30
    with a.output.open('x', buffering=1) as out:
        out.write(json.dumps({'metadata':header})+'\n')
        for sample in range(a.samples):
            for index, case in enumerate(plan['cases']):
                commands = case['commands'] if (sample+index)%2==0 else list(reversed(case['commands']))
                for command in commands:
                    remaining=deadline-time.monotonic()
                    if remaining <= 0.05:
                        out.write(json.dumps({'status':'budget_exhausted','seconds':time.monotonic()-start})+'\n')
                        return
                    env=base_env.copy()
                    env.update(command.get('env',{}))
                    t=time.monotonic()
                    record={'sample':sample+1, 'case':case['name'], 'version':command['version'],
                            'argv':command['argv'], 'env_overrides':command.get('env',{})}
                    try:
                        proc=subprocess.run(command['argv'], capture_output=True, text=True, env=env,
                                            timeout=min(a.case_timeout,remaining), check=False)
                        record.update(status='completed' if proc.returncode==0 else 'failed',
                                      returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
                    except subprocess.TimeoutExpired as error:
                        def string(x): return x.decode(errors='replace') if isinstance(x,bytes) else (x or '')
                        record.update(status='timeout',stdout=string(error.stdout),stderr=string(error.stderr))
                    record['process_seconds']=time.monotonic()-t
                    out.write(json.dumps(record)+'\n')
        out.write(json.dumps({'status':'completed_schedule','seconds':time.monotonic()-start})+'\n')

if __name__=='__main__':
    main()
