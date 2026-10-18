"""Run ROM-bound assisted routes and compare separate native/interpreter surfaces.

This compares the shared runtime's native and interpreter paths. It does not
certify independent hardware parity or complete CPU/WRAM equivalence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zlib

p=argparse.ArgumentParser(description=__doc__)
for name in ('runtime','rom','suite','out'):p.add_argument('--'+name,type=Path,required=True)
args=p.parse_args()
runtime=args.runtime.resolve();rom=args.rom.resolve();suite_path=args.suite.resolve();out=args.out.resolve()
suite=json.loads(suite_path.read_text(encoding='utf-8'));data=rom.read_bytes()
if (f'{zlib.crc32(data):08X}',len(data))!=(suite['rom_crc32'].upper(),suite['rom_size']):raise SystemExit('Unsupported suite ROM')
out.mkdir(parents=True,exist_ok=False)
report=dict(schema=1,rom_sha256=hashlib.sha256(data).hexdigest(),runtime_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),cases=[],scope=__doc__.strip())
environment=dict(os.environ,SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
options=dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name=='nt' else {}
for case in suite['cases']:
 name=case['name']
 if not name or Path(name).name!=name or name in ('.','..'):raise ValueError('Invalid case name')
 row=dict(name=name,modes={},comparisons=[])
 source=(suite_path.parent/case['route']).resolve()
 captures=[line.split(maxsplit=2)[2] for line in source.read_text().splitlines() if len(line.split())>=3 and line.split()[1]=='capture']
 for mode in ('native','interpreter'):
  directory=out/name/mode;directory.mkdir(parents=True)
  route=directory/'route.txt';shutil.copyfile(source,route)
  command=[str(runtime),str(rom),'--route',str(route),'--frames',str(case['frames']),'--report',str(directory/'execution.json'),'--audio-wav',str(directory/'audio.wav')]
  if mode=='interpreter':command.append('--interp')
  with (directory/'run.log').open('w',encoding='utf-8') as log:subprocess.run(command,cwd=directory,env=environment,stdout=log,stderr=subprocess.STDOUT,check=True,**options)
  row['modes'][mode]=json.loads((directory/'execution.json').read_text())
 for capture in captures:
  planes={}
  for suffix in ('.png','.vram','.cram','.regs'):
   a=(out/name/'native'/(capture+suffix)).read_bytes();b=(out/name/'interpreter'/(capture+suffix)).read_bytes()
   planes[suffix]=dict(equal=a==b,sha256=[hashlib.sha256(x).hexdigest() for x in (a,b)])
  row['comparisons'].append(dict(capture=capture,planes=planes))
 row['zero_fallback']=row['modes']['native']['dispatch_misses']==0 and row['modes']['native']['interpreted_cycles']==0
 report['cases'].append(row);(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 print(name,'zero fallback:',row['zero_fallback'],'equal surface captures:',sum(all(v['equal'] for v in c['planes'].values()) for c in row['comparisons']),'/',len(captures),flush=True)
if not all(c['zero_fallback'] for c in report['cases']):raise SystemExit(1)
