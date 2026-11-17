#!/usr/bin/env python3
"""Seal or verify the copied baseline and its two built consumers."""
import hashlib
import json
import os
from pathlib import Path

e=Path('build/collection-execution-fix-2026-11-17').absolute()
def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
copy=json.loads((e/'baseline-copy-identities.json').read_text())
for name,x in copy.items():
    p=Path(name)
    if p.stat().st_size!=x['bytes'] or sha(p)!=x['sha256']:raise RuntimeError('copied baseline changed: '+name)
extra=[e/'baseline-consumer/execution-time',e/'baseline-consumer/execution-count',e/'baseline-consumer/CMakeCache.txt',
       e/'baseline-configure.log',e/'baseline-build.log',e/'baseline-source.tar']
for p in extra:copy[str(p)]={'bytes':p.stat().st_size,'sha256':sha(p)}
path=e/'baseline-frozen.json'
if path.exists():
    if json.loads(path.read_text())!=copy:raise RuntimeError('sealed baseline differs; preserve existing evidence')
    print(json.dumps({'status':'verified','frozen_inputs':len(copy)}));raise SystemExit
with path.open('x') as f:json.dump(copy,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
state_path=e/'state.json';state=json.loads(state_path.read_text())
if state['status']!='baseline_copied':raise RuntimeError('baseline state differs')
state.update(status='baseline_ready',baseline_frozen_inputs=len(copy),
             windows={'screen':{'status':'not_started','limit_seconds':30},'repeat':{'status':'not_started','limit_seconds':30}})
with state_path.open('w') as f:json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
fd=os.open(e,os.O_DIRECTORY);os.fsync(fd);os.close(fd)
print(json.dumps({'status':'baseline_ready','frozen_inputs':len(copy)}))
