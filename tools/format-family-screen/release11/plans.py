#!/usr/bin/env python3
"""Freeze the three approved <=30 s windows; execute with existing window.py."""
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
    p.add_argument('--cpp', type=Path, required=True)
    p.add_argument('--prefix', type=Path, required=True)
    p.add_argument('--python', type=Path, required=True)
    a = p.parse_args()
    root = Path.cwd()
    out = a.evidence.absolute()
    source = root / 'tools/format-family-screen/release11'
    spatial = root / 'build/format-family-io/fixtures-family-v3/spatialdata.zarr'
    multi = out / 'fixtures/multichunk.zarr'
    small = root / 'build/format-family-io/fixtures-family-v3/dataset.h5ads'
    large = root / 'build/format-family-repair/screen/fixtures/10000/collection.h5ads'
    fixtures = [p for directory in (spatial, multi) for p in directory.rglob('*') if p.is_file()]
    for inventory, prefix in [(root/'build/format-family-io/fixtures-family-v3/inventory.json', root/'build/format-family-io/fixtures-family-v3'),
                              (root/'build/format-family-repair/screen/fixtures/inventory.json', root/'build/format-family-repair/screen/fixtures')]:
        data = json.loads(inventory.read_text())
        for name, entry in data['files'].items():
            # Freeze only the requested small family stores and 10k collection.
            if inventory.parent.name == 'fixtures-family-v3':
                include = name.startswith(('spatialdata.zarr/', 'dataset'))
            else:
                include = name.startswith('10000/') and not name.endswith('monolithic.h5ad')
            if not include:
                continue
            file = prefix/name
            if sha(file) != entry['sha256'] or file.stat().st_size != entry['bytes']:
                raise RuntimeError(f'changed fixture: {file}')
            fixtures.append(file)
    identities = sorted(set(fixtures + list(source.glob('*.py')) + list(source.glob('*.cpp')) +
        [source/'CMakeLists.txt', source/'io.c', root/'tools/format-family-screen/python_family.py',
         root/'tools/format-family-screen/window.py', a.cpp.absolute(), a.python.absolute(),
         a.prefix.absolute()/'lib/libanndatapp.a', a.prefix.absolute()/'lib/libanndatapp_core.a',
         out/'librelease11_io.so', out/'librelease11_alloc.so']))
    inventory = {str(file): {'bytes': file.stat().st_size, 'sha256': sha(file)} for file in identities}
    (out/'input-identities.json').write_text(json.dumps(inventory, indent=2)+'\n')

    def command(version, case, fixture, diagnostic=False):
        argv = ([str(a.cpp.absolute()), case, str(fixture)] if version == 'cpp' else
                [str(a.python.absolute()), str(source/'python_probe.py'), case, str(fixture)])
        if diagnostic:
            argv.append('--diagnostic')
        env = {'ANNDATAPP_INTEROP_ZARR_WAKEUP': '0'}
        if diagnostic:
            env['LD_PRELOAD'] = f'{out}/librelease11_alloc.so:{out}/librelease11_io.so'
        return {'version': version, 'argv': argv, 'env': env}

    rasters = [('raster-decimated', spatial), ('raster-aligned', spatial), ('raster-multichunk', multi)]
    collections = [('dataset-small', small), ('dataset-10k', large)]
    timing_raster, timing_collection, diagnostic = [], [], []
    # One screening pair for every primary case before repeats. Raw controls
    # get one pair only; primary comparisons get at most three observations.
    for repeat in range(1, 4):
        for name, fixture in rasters:
            timing_raster.append({'name': f'{name}@{repeat}', 'commands':
                [command(version, name, fixture) for version in ('cpp', 'python')]})
        if repeat == 1:
            for name, fixture in rasters:
                timing_raster.append({'name': f'{name}-raw@1', 'commands':
                    [command(version, name+'-raw', fixture) for version in ('cpp', 'python')]})
        for name, fixture in collections:
            timing_collection.append({'name': f'{name}@{repeat}', 'commands':
                [command('cpp', name, fixture), command('python', name, fixture),
                 command('python-selected-axis-checks', name+'-validated', fixture)]})
    for name, fixture in rasters:
        for suffix in ('', '-raw'):
            diagnostic.append({'name': name+suffix, 'commands':
                [command(version, name+suffix, fixture, True) for version in ('cpp', 'python')]})
    for name, fixture in collections:
        diagnostic.append({'name': name, 'commands':
            [command('cpp', name, fixture, True), command('python', name, fixture, True),
             command('python-selected-axis-checks', name+'-validated', fixture, True)]})
    for name, cases in [('raster', timing_raster), ('collection', timing_collection), ('diagnostic', diagnostic)]:
        plan = {'description': 'Release1.1 bounded selected-output comparison; @N is the replicate; execute --samples1. No timing evidence from diagnostics.',
                'identity_files': [str(f) for f in identities], 'cases': cases}
        with (out/f'{name}-plan.json').open('x') as f:
            json.dump(plan, f, indent=2)
            f.write('\n')
    print('Frozen plans and fixture/source/binary identities:', out)


if __name__ == '__main__':
    main()
