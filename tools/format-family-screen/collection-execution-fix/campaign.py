#!/usr/bin/env python3
"""Run one frozen window once; preserve partial evidence and enforce a wall cap."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
t=time.monotonic()
p=argparse.ArgumentParser();p.add_argument('window',choices=('screen','repeat'));p.add_argument('evidence',type=Path);a=p.parse_args()
e=a.evidence.absolute();path=e/'state.json';state=json.loads(path.read_text());window=state['windows'][a.window]
if window['status']!='not_started' or state['status'] not in ('ready','checkpointed'):
    raise SystemExit('window already started or campaign not ready; preserve evidence')
if a.window=='repeat' and state['windows']['screen']['status']!='complete':
    raise SystemExit('first screen did not complete; preserve its partial evidence')
for suffix in ('receipt.jsonl','runner.stdout','runner.stderr'):
    if (e/(a.window+'-'+suffix)).exists():raise SystemExit('window output already exists; preserve evidence')
def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
plan=e/(a.window+'-plan.json');manifest=e/'frozen-inputs.json'
if sha(plan)!=state['plan_sha256'][a.window] or sha(manifest)!=state['frozen_inputs_sha256']:
    raise SystemExit('sealed plan or input manifest changed; no workload started')
for name,x in json.loads(manifest.read_text()).items():
    p=Path(name)
    if p.stat().st_size!=x['bytes'] or sha(p)!=x['sha256']:raise SystemExit('frozen input changed; no workload started: '+name)
remaining=29-(time.monotonic()-t)  # Reserve a second for checkpointing and process cleanup.
if remaining<=0:raise SystemExit('preflight consumed the window budget; no workload started')
def save():
    with path.open('w') as f:json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
    fd=os.open(e,os.O_DIRECTORY);os.fsync(fd);os.close(fd)
window.update(status='running',started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());state['status']='running';save()
cmd=[sys.executable,str(Path(__file__).parent/'stop_on_error.py'),'--plan',str(e/(a.window+'-plan.json')),
     '--output',str(e/(a.window+'-receipt.jsonl')),'--case-timeout','5']
remaining=29-(time.monotonic()-t)
if remaining<=0:
    window.update(status='partial_or_failed',error='checkpoint consumed remaining time');state['status']='checkpointed';save()
    raise SystemExit('checkpoint consumed remaining window budget; no workload started')
proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
try: stdout,stderr=proc.communicate(timeout=remaining)
except subprocess.TimeoutExpired:
    try:os.killpg(proc.pid,signal.SIGKILL)
    except ProcessLookupError:pass
    stdout,stderr=proc.communicate();window['timeout']=True
elapsed=time.monotonic()-t
for suffix,content in (('runner.stdout',stdout),('runner.stderr',stderr)):
    with (e/(a.window+'-'+suffix)).open('x') as f:f.write(content);f.flush();os.fsync(f.fileno())
receipt=e/(a.window+'-receipt.jsonl');rows=[]
if receipt.exists():
    for i,line in enumerate(receipt.read_text(errors='replace').splitlines(),1):
        try:rows.append(json.loads(line))
        except json.JSONDecodeError:window['malformed_receipt_line']=i;break
complete=bool('malformed_receipt_line' not in window and proc.returncode==0 and rows and rows[-1].get('status')=='completed_schedule' and all(x.get('status')=='completed' for x in rows[1:-1]))
elapsed=time.monotonic()-t
window.update(status='complete' if complete else 'partial_or_failed',returncode=proc.returncode,wall_seconds=elapsed,
              measured_seconds=rows[-1].get('seconds') if rows else None,completed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
state['status']='complete' if all(w['status']=='complete' for w in state['windows'].values()) else 'checkpointed';save()
print(json.dumps({'window':a.window,'status':window['status'],'wall_seconds':elapsed,'measured_seconds':window['measured_seconds']}))
if not complete:raise SystemExit('window incomplete; preserve receipts and stop for investigation')
