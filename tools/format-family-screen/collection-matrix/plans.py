#!/usr/bin/env python3
"""Freeze three bounded serial windows after fixture/consumer admission."""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('evidence', type=Path)
    p.add_argument('--prefix', type=Path, required=True)
    a = p.parse_args()
    root = Path.cwd(); evidence = a.evidence.absolute(); prefix = a.prefix.absolute()
    fixture_root = evidence/'fixtures-complete'
    inventory = json.loads((fixture_root/'inventory.json').read_text())
    for name, expected in inventory['files'].items():
        path = fixture_root/name
        if path.stat().st_size != expected['bytes'] or sha(path) != expected['sha256']:
            raise RuntimeError('fixture identity differs: '+str(path))
    sources = root/'tools/format-family-screen/collection-matrix'
    py = root/'build/reference/full/bin/python3'
    cpp = evidence/'consumer/collection-matrix-cpp'
    python_probe = sources/'python_probe.py'
    existing = root/'build/collection-preparation-2026-11-16/candidate-profile-v2/build'
    metadata = root/'build/collection-prep-fix-2026-11-16/comparison/libmetadata.so'
    io = existing/'libcollection-io.so'; alloc = existing/'libcollection-alloc.so'
    frozen = [*sorted(sources.glob('*.*')), root/'tools/format-family-screen/collection-fix/cpp_probe.cpp', root/'tools/format-family-screen/window.py',root/'tests/reference/requirements-full.lock', cpp, py,
              prefix/'lib/libanndatapp.a',prefix/'lib/libanndatapp_core.a', metadata,io,alloc,
              fixture_root/'inventory.json',fixture_root/'metadata.json',evidence/'generation-state.json',evidence/'semantic-admission.json',evidence/'environment.json',evidence/'consumer/CMakeCache.txt',
              *[fixture_root/name for name in inventory['files']]]
    frozen += sorted((prefix/'share/anndatapp/modules').glob('*.cppm'))
    frozen += sorted((root/'build/reference/full/lib/python3.12/site-packages/snapatac2').glob('*.so'))
    for path in frozen:
        if not path.is_file(): raise RuntimeError('missing input: '+str(path))
    if sha(prefix/'lib/libanndatapp.a') != '2678592fc437c3744915c0672943c458a7b7dd2840e113d1f1a4a67abe4be930':
        raise RuntimeError('prefix does not match qualified optional-axis library')
    cases_root = evidence/'cases'; cases_root.mkdir(exist_ok=False)
    spread = lambda n,k: [(n-1)*i//(k-1) for i in range(k)]
    specs = {}
    def spec(name, fixture, rows, cols, repeats=1):
        nnz = 128 if fixture == 'higher-density-c8' else 16
        item = dict(name=name,manifest=str(fixture_root/fixture/'collection.h5ads'),rows=rows,cols=cols,repeat=repeats,shape=[8192,2048],nnz_per_row=nnz)
        js = cases_root/(name+'.json'); tx=cases_root/(name+'.txt')
        js.write_text(json.dumps(item,indent=2)+'\n')
        tx.write_text(f'{name}\n{item["manifest"]}\n8192 2048 {nnz}\n{len(rows)} {len(cols)} {repeats}\n'+ ' '.join(map(str,rows))+'\n'+' '.join(map(str,cols))+'\n')
        frozen.extend((js,tx)); specs[name]=(js,tx)
        return name
    common_rows, common_cols = spread(8192,64),spread(2048,64)
    screen = [spec('components-'+str(n),'sparse-c'+str(n),common_rows,common_cols) for n in (1,8,32)]
    screen += [spec('positive-stride','sparse-c8',list(range(0,8192,128)),list(range(0,2048,32))),
               spec('contiguous-one-component','sparse-c8',list(range(256)),list(range(128))),
               spec('contiguous-across-boundary','sparse-c8',list(range(896,1152)),list(range(128))),
               spec('reverse-duplicates','sparse-c8',[8191,0,8191,1024,1023,4096,0,17]*8,[2047,0,127,0,254,17,382,127]*8)]
    volume = [spec('all-rows-spread-columns','sparse-c8',list(range(8192)),common_cols),
              spec('all-columns-512-rows','sparse-c8',spread(8192,512),list(range(2048))),
              spec('higher-density-all-columns','higher-density-c8',spread(8192,512),list(range(2048))),
              spec('gzip-spread','gzip-c8',common_rows,common_cols)]
    reuse = [spec('reuse-sparse','sparse-c8',common_rows,list(range(2048)),5),
             spec('reuse-higher-density','higher-density-c8',spread(8192,256),list(range(2048)),5)]
    env={'ANNDATAPP_INTEROP_ZARR_WAKEUP':'0'}
    def command(version,name,diagnostic=False):
        js,tx=specs[name]
        argv=([str(py),str(python_probe),str(js)] if version=='python' else [str(cpp),str(tx)])
        if version=='checked': argv.append('--check-axis-names')
        cfg=dict(env)
        if diagnostic:
            argv.append('--diagnostic'); cfg['LD_PRELOAD']=':'.join(map(str,(metadata,io,alloc)))
        return dict(version=version,argv=argv,env=cfg)
    def write_window(window,observations,auxiliary=False):
        cases=[]
        for index,(name,repeat) in enumerate(observations):
            versions=['default','python'] if (repeat+index)%2 else ['python','default']
            # The serial runner reverses alternate cases; compensate so
            # actual process order alternates by observation and geometry.
            if len(cases)%2: versions.reverse()
            cases.append(dict(name=f'{name}@{repeat}',commands=[command(v,name) for v in versions]))
        if auxiliary:
            for name in ('components-8','higher-density-all-columns'):
                for repeat in range(3):
                    cases.append(dict(name=f'{name}-checked@{repeat+1}',commands=[command('checked',name)]))
            for name in ('components-8','higher-density-all-columns'):
                cases.append(dict(name=name+'-diagnostic',commands=[command('default',name,True)]))
        out=dict(description=f'Collection matrix {window}; <=30 s including subprocess startup, serial normal-cache operations',identity_files=[str(x) for x in frozen],cases=cases)
        with (evidence/(window+'-plan.json')).open('x') as f: json.dump(out,f,indent=2);f.write('\n')
    screen_observations=[(name,repeat) for repeat in range(1,4) for name in screen[:3]] + [(name,1) for name in screen[3:]]
    volume_observations=[(name,repeat) for repeat in range(1,4) for name in volume[1:3]] + [(name,1) for name in (volume[0],volume[3])]
    reuse_observations=[(name,repeat) for repeat in range(1,4) for name in reuse] + [(screen[4],repeat) for repeat in (2,3)]
    write_window('screen',screen_observations)
    write_window('volume',volume_observations)
    write_window('reuse',reuse_observations,True)
    manifest={str(path):dict(bytes=path.stat().st_size,sha256=sha(path)) for path in frozen}
    with (evidence/'frozen-inputs.json').open('x') as f: json.dump(manifest,f,indent=2);f.write('\n')
    print(json.dumps(dict(cases={k:len(v) for k,v in [('screen',screen),('volume',volume),('reuse',reuse)]},frozen_files=len(frozen),new_fixture_bytes=inventory['bytes'])))

if __name__=='__main__': main()
