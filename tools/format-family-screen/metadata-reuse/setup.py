#!/usr/bin/env python3
"""Stage qualified probe helpers and wrappers without executing workloads."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--repo',type=Path,default=Path.cwd())
p.add_argument('--source-dir',type=Path,required=True)
a=p.parse_args(); repo=a.repo.absolute(); dest=a.source_dir.absolute(); dest.mkdir(parents=True,exist_ok=True)
tool=repo/'tools/format-family-screen/metadata-reuse'
qualified=(repo/'tools/format-family-screen/collection-fix/cpp_probe.cpp').read_bytes()
marker=b'\nint main(int argc, char** argv) {'
if qualified.count(marker)!=1: raise RuntimeError('qualified probe main marker changed')
outputs={'reference_cpp_probe.cpp':qualified.split(marker)[0]}
outputs.update({name:(tool/name).read_bytes() for name in ('small_collection_cpp.cpp','control_cpp.cpp','CMakeLists.txt')})
for name,data in outputs.items():
 target=dest/name
 if target.exists():
  if target.read_bytes()!=data: raise RuntimeError('existing source differs: '+str(target))
 else:target.write_bytes(data)
print('Staged four reproducible source files in',dest)
