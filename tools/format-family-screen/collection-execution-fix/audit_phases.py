#!/usr/bin/env python3
"""Compare every common C++ timing phase from the already sealed screen."""
import hashlib
import json
from pathlib import Path
import sys

e=Path(sys.argv[1]).absolute();p=e/'summary.json'
with p.open('rb') as f:summary_sha=hashlib.file_digest(f,'sha256').hexdigest()
if summary_sha!='f3ab409d3881e58654ec3d400468af4cbb9f29fb932003e7f8b4b59599ee4805':
    raise RuntimeError('summary does not match the completed campaign')
summary=json.loads(p.read_text());phases={};regressions=[]
for name,case in summary['cases'].items():
    before,after=case['modes']['baseline'],case['modes']['candidate'];rows={}
    for key in ('open','selection','preparation_requirements','buffer_allocation','transfer','end_to_end','reused_operation'):
        if key not in before or key not in after:continue
        a,b=before[key]['median_ms'],after[key]['median_ms'];delta=b-a
        material=bool(a and abs(delta)>0.1 and abs(delta)/a>0.1)
        row={'baseline_ms':a,'candidate_ms':b,'delta_ms':delta,'relative_change':delta/a if a else None,
             'baseline_samples_ms':before[key]['samples_ms'],'candidate_samples_ms':after[key]['samples_ms'],
             'material':material,'observations':case['observations']}
        rows[key]=row
        if material and delta>0:regressions.append({'case':name,'phase':key,**row})
    phases[name]=rows
with Path(__file__).open('rb') as f:script_sha=hashlib.file_digest(f,'sha256').hexdigest()
out={'source_summary_sha256':summary_sha,'analysis_script_sha256':script_sha,'threshold':'absolute delta >0.1 ms AND relative delta >10%',
     'note':'Post-campaign analysis only; medians from distinct phases need not add to the end-to-end median. Controls have one observation.',
     'regressions':regressions,'phases':phases}
with (e/'phase-audit.json').open('x') as f:json.dump(out,f,indent=2);f.write('\n')
print(json.dumps({'regressions':regressions,'cases':len(phases)},indent=2))
