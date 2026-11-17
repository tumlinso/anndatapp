#!/usr/bin/env python3
"""Checkpoint, run at most one 30-second diagnostic, and preserve partial results."""
import datetime
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

e=Path(sys.argv[1]).absolute(); path=e/'state.json';state=json.loads(path.read_text())
if state.get('status') != 'ready' or state.get('timing_window') != 'not_started':
    raise SystemExit('already started or not ready; preserve evidence')
def save():
    with path.open('w') as f:json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
    fd=os.open(e,os.O_DIRECTORY);os.fsync(fd);os.close(fd)
state.update(status='running',timing_window='running',started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());save()
runner=Path(__file__).parent.parent/'window.py';t=time.monotonic()
proc=subprocess.Popen([sys.executable,str(runner),'--plan',str(e/'plan.json'),'--output',str(e/'receipt.jsonl'),'--case-timeout','5'],
                      stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
try: stdout,stderr=proc.communicate(timeout=30)
except subprocess.TimeoutExpired:
    os.killpg(proc.pid,signal.SIGKILL);stdout,stderr=proc.communicate();state['timeout']=True
elapsed=time.monotonic()-t
(e/'runner.stdout').write_text(stdout);(e/'runner.stderr').write_text(stderr)
receipt=e/'receipt.jsonl';lines=[json.loads(line) for line in receipt.read_text().splitlines()] if receipt.exists() else []
complete=bool(proc.returncode==0 and lines and lines[-1].get('status')=='completed_schedule' and
              all(r.get('status')=='completed' for r in lines[1:-1]))
state.update(status='complete' if complete else 'partial_or_failed',timing_window='complete' if complete else 'partial_or_failed',
             returncode=proc.returncode,runner_wall_seconds=elapsed,completed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
             measured_seconds=lines[-1].get('seconds') if lines else None)
save()
print(json.dumps({'status':state['status'],'runner_wall_seconds':elapsed,'measured_seconds':state['measured_seconds']}))
if not complete: raise SystemExit('incomplete diagnostic; preserve and inspect partial results')
