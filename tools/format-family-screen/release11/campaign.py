#!/usr/bin/env python3
"""Run each approved window once and checkpoint before launching any process."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def save(path, state):
    pending = path.with_suffix('.next')
    with pending.open('w') as f:
        json.dump(state, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    pending.replace(path)
    fd = os.open(path.parent, os.O_DIRECTORY)
    os.fsync(fd)
    os.close(fd)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('window', choices=('raster', 'collection', 'diagnostic'))
    p.add_argument('evidence', type=Path)
    a = p.parse_args()
    base = a.evidence.absolute()
    state_path = base/'campaign-state.json'
    state = json.loads(state_path.read_text())
    window = state['windows'][a.window]
    if window['status'] != 'not_started':
        raise RuntimeError('Window already started; preserve its receipts and interpret before any restart.')
    window.update(status='running', started_utc=datetime.now(timezone.utc).isoformat())
    state['status'] = 'measuring'
    save(state_path, state)
    output = base/f'{a.window}-results.jsonl'
    runner = Path(__file__).parent.parent/'window.py'
    proc = subprocess.run([sys.executable, str(runner), '--plan', str(base/f'{a.window}-plan.json'),
                           '--output', str(output), '--samples', '1', '--case-timeout', '10'])
    window['finished_utc'] = datetime.now(timezone.utc).isoformat()
    window['returncode'] = proc.returncode
    if output.exists():
        raw = output.read_bytes()
        window['results_sha256'] = hashlib.sha256(raw).hexdigest()
        rows = [json.loads(line) for line in raw.splitlines()]
        final = rows[-1]
        window['seconds'] = final.get('seconds')
        window['status'] = final.get('status', 'partial') if proc.returncode == 0 else 'failed'
        window['process_records'] = sum('version' in row for row in rows)
        window['failed_records'] = sum(row.get('status') in ('failed', 'timeout') for row in rows)
    else:
        window['status'] = 'failed_before_results'
    save(state_path, state)
    print(json.dumps(window, sort_keys=True))
    raise SystemExit(proc.returncode)


if __name__ == '__main__':
    main()
