#!/usr/bin/env python3
"""Copy and identify the verified pre-execution-fix prefix and existing probes."""
import hashlib
import json
import os
from pathlib import Path
import shutil

root=Path.cwd(); e=root/'build/collection-execution-fix-2026-11-17'
old=Path('/home/tumlinson/anndatapp/build/collection-metadata-reuse-qualification-2026-11-17/candidate-prefix')
source=e/'source'; source.mkdir(exist_ok=False)
for directory,names in {
    'collection-execution-fix':['CMakeLists.txt'],
    'collection-matrix':['cpp_probe.cpp','python_probe.py'],
    'collection-fix':['cpp_probe.cpp','metadata.c'],
    'collection-overhead':['cpp_probe.cpp','handles.c'],
    'collection-preparation':['io.c','alloc.cpp'],
}.items():
    dst=source/directory;dst.mkdir()
    for name in names: shutil.copy2(root/'tools/format-family-screen'/directory/name,dst/name)
shutil.copy2(root/'tools/format-family-screen/window.py',source/'window.py')
shutil.copytree(old,e/'baseline-prefix')
libs=e/'interposers';libs.mkdir()
for p in [root/'build/collection-prep-fix-2026-11-16/comparison/libmetadata.so',
          root/'build/collection-overhead-2026-11-17/libhandles.so',
          root/'build/collection-preparation-2026-11-16/candidate-profile-v2/build/libcollection-io.so',
          root/'build/collection-preparation-2026-11-16/candidate-profile-v2/build/libcollection-alloc.so']:
    shutil.copy2(p,libs/p.name)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
archive=e/'baseline-prefix/lib/libanndatapp.a'; core=e/'baseline-prefix/lib/libanndatapp_core.a'
assert sha(archive)=='91aa9e1ea7fa4fad36f945e715255ceb7d5f69cab55a179e7f46ae9c77948b46'
assert sha(core)=='2b37603ef8c56078002abf05c5dd7ba2a295196ffe15bae5ed45d2554caeef19'
files=sorted(p for folder in (source,e/'baseline-prefix',libs) for p in folder.rglob('*') if p.is_file())
state={'status':'baseline_copied','baseline_source_commit':'5d21232','baseline_production_commit':'d479d5d',
       'baseline_archive_sha256':sha(archive),'core_archive_sha256':sha(core),'baseline_files':len(files),
       'source_copy':str(source),'window_limit_seconds':30,'windows':{'screen':'not_started','repeat':'not_started'},
       'new_fixtures':0,'normal_caches':True}
with (e/'state.json').open('x') as f:json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
with (e/'baseline-copy-identities.json').open('x') as f:json.dump({str(p):{'bytes':p.stat().st_size,'sha256':sha(p)} for p in files},f,indent=2);f.write('\n')
print(json.dumps(state))
