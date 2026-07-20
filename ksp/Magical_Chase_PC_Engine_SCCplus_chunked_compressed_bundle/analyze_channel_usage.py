#!/usr/bin/env python3
from pathlib import Path
import struct,json,csv
BUILD=Path('/mnt/data/magical_chase_ksp_build')
OUT=Path('/mnt/data/magical_chase_sccplus_ksp/channel_usage.csv')
rows=[]
for i in range(19):
 d=(BUILD/f'{i:02d}.bin').read_bytes();m=json.loads((BUILD/f'{i:02d}.json').read_text())
 frames=struct.unpack_from('<H',d,6)[0];stream=struct.unpack_from('<H',d,10)[0]
 p=stream;wait=d[p];p+=1;vol=[0]*7;active=[0]*7;all5=all6=0;maxa=0
 def event(p):
  fm,vm,wm=d[p:p+3];p+=3
  p+=sum((wm>>x)&1 for x in range(5))
  p+=2*sum((fm>>x)&1 for x in range(6))+((fm>>6)&1)
  for x in range(7):
   if vm>>x&1:vol[x]=d[p]&15;p+=1
  delay=d[p];p+=1
  return p,delay
 if wait==0:p,wait=event(p)
 for f in range(frames):
  for c,v in enumerate(vol):active[c]+=bool(v)
  n=sum(bool(v) for v in vol);maxa=max(maxa,n);all5+=n>=5;all6+=n>=6
  if wait:wait-=1
  else:
   if d[p]==0xFF:break
   p,wait=event(p)
 rows.append({'track':i,'id':f"0x{m['id']:02X}",'title':m['title'],'frames':frames,'max_simultaneous':maxa,'frames_5plus':all5,'frames_6':all6,**{f'ch{c+1}_active_pct':round(active[c]*100/frames,2) for c in range(7)}})
with OUT.open('w',newline='',encoding='utf-8') as f:
 w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
print(json.dumps({'tracks':19,'tracks_using_sixth_voice':sum(r['ch6_active_pct']>0 for r in rows),'tracks_with_six_simultaneous':sum(r['frames_6']>0 for r in rows),'max_simultaneous':max(r['max_simultaneous'] for r in rows)},indent=2))
