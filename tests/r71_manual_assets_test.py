from pathlib import Path
import sys,hashlib
r=Path(__file__).resolve().parents[1]
paths=sorted((r/'manual').glob('spread??.rgb565'))
if not paths:
 print('Optional manual assets absent: manual disabled; no scans distributed')
 sys.exit(0)
assert len(paths)==10,len(paths)
for i,p in enumerate(paths):
 assert p.name==f'spread{i:02d}.rgb565'
 assert p.stat().st_size==1024*760*2,(p,p.stat().st_size)
 raw=p.read_bytes()
 assert len(set(raw))>40,p
 print(p.name,len(raw),hashlib.sha256(raw).hexdigest()[:16])
print('R72 high resolution original scans: 10 spreads PASS')
