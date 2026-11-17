#!/usr/bin/env python3
"""Checkpoint a single authorized 30-second window; never restart it silently."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys


def save(path,state):
    with path.open('w') as f:
        json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
    directory=os.open(path.parent,os.O_DIRECTORY)
    try: os.fsync(directory)
    finally: os.close(directory)


def main():
    p=argparse.ArgumentParser();p.add_argument('window',choices=('screen','volume','reuse'));p.add_argument('evidence',type=Path);a=p.parse_args()
    evidence=a.evidence.absolute();path=evidence/'state.json';state=json.loads(path.read_text());window=state['timing_windows'][a.window]
    if window['status']!='not_started':raise RuntimeError('preserve the started window; no automatic restart')
    window.update(status='running',started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    state.update(status='measuring',timing_started=True);save(path,state)
    runner=Path(__file__).parent.parent/'window.py'
    result=subprocess.run([sys.executable,str(runner),'--plan',str(evidence/(a.window+'-plan.json')),'--output',str(evidence/(a.window+'-receipt.jsonl')),'--case-timeout','5'],check=False)
    receipt=evidence/(a.window+'-receipt.jsonl')
    lines=[json.loads(line) for line in receipt.read_text().splitlines()] if receipt.exists() else []
    completed=bool(lines and lines[-1].get('status')=='completed_schedule' and all(x.get('status')=='completed' for x in lines[1:-1]))
    window.update(status='complete' if completed else 'partial_or_failed',returncode=result.returncode,completed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),measured_seconds=lines[-1].get('seconds') if lines else None)
    state['status']='complete' if all(w['status']=='complete' for w in state['timing_windows'].values()) else 'checkpointed'
    save(path,state)
    if not completed:raise RuntimeError('window incomplete; inspect preserved receipts before any next decision')

if __name__=='__main__':main()
