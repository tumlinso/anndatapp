#!/usr/bin/env python3
"""Reuse the frozen window runner, stopping after its first failed process."""
import importlib.util
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[3]
path=root/'build/collection-execution-fix-2026-11-17/source/window.py'
spec=importlib.util.spec_from_file_location('collection_window',path)
window=importlib.util.module_from_spec(spec);spec.loader.exec_module(window)
run=window.subprocess.run
failed=False
class PreviousFailure(Exception):pass

def guarded(*args,**kwargs):
    global failed
    if failed:raise PreviousFailure
    try:result=run(*args,**kwargs)
    except subprocess.TimeoutExpired:
        failed=True
        raise
    if result.returncode:failed=True
    return result

window.subprocess.run=guarded
try:window.main()
except PreviousFailure:
    print('Stopped after the first failed or timed-out process; the partial receipt is retained.',file=sys.stderr)
    raise SystemExit(2)
