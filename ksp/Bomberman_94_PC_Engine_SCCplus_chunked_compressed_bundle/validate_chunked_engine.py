#!/usr/bin/env python3
from pathlib import Path
import struct,json
BASE=Path('/mnt/data/bomberman94_sccplus_ksp')
KSP=(BASE/'Bomberman_94_PC_Engine_SCCplus_chunked_prototype.ksp').read_bytes()
ENGINE=(BASE/'engine.bin').read_bytes()
EMAP=json.loads((BASE/'engine_map.json').read_text())
INIT=int(EMAP['labels']['init'],16);PLAY=int(EMAP['labels']['play'],16);GATE=int(EMAP['page_gateway'],16)
PTR=0x4700;WAIT=0x4702;ENDED=0x4703;TRACK=0x4800
PAGES=[]
pos=0x4231
page_count=KSP[0x21+5]
track_count=KSP[0x21+6]
for _ in range(page_count):
    sz=struct.unpack_from('<H',KSP,pos)[0];pos+=2
    end=pos+sz
    records=[]
    while pos<end:
        off,n=struct.unpack_from('<HH',KSP,pos);pos+=4
        records.append((off,KSP[pos:pos+n]));pos+=n
    assert pos==end
    assert len(records)==1 and records[0][0]==0x800
    PAGES.append(records[0][1])
assert pos==len(KSP)
first_pages=list(KSP[0x21+0x110:0x21+0x110+track_count])

class CPU:
 def __init__(self,page):
  self.m=bytearray(65536);self.m[0x4000:0x4000+len(ENGINE)]=ENGINE;self.load_page(page)
  self.a=self.b=self.c=self.d=self.e=self.h=self.l=0;self.sp=0xF000;self.pc=0;self.z=False;self.ports={};self.psg_sel=0;self.steps=0;self.switches=[]
 def load_page(self,page):
  b=PAGES[page];self.m[TRACK:TRACK+len(b)]=b;self.current_page=page;self.switches.append(page) if hasattr(self,'switches') else None
 def hl(self):return self.h<<8|self.l
 def de(self):return self.d<<8|self.e
 def bc(self):return self.b<<8|self.c
 def sethl(self,v):self.h=(v>>8)&255;self.l=v&255
 def setde(self,v):self.d=(v>>8)&255;self.e=v&255
 def setbc(self,v):self.b=(v>>8)&255;self.c=v&255
 def push(self,v):self.sp=(self.sp-1)&65535;self.m[self.sp]=(v>>8)&255;self.sp=(self.sp-1)&65535;self.m[self.sp]=v&255
 def pop(self):v=self.m[self.sp]|self.m[(self.sp+1)&65535]<<8;self.sp=(self.sp+2)&65535;return v
 def w(self,addr,v):self.m[addr&65535]=v&255
 def rdw(self,addr):return self.m[addr]|self.m[(addr+1)&65535]<<8
 def wrw(self,addr,v):self.w(addr,v);self.w(addr+1,v>>8)
 def run_call(self,addr,maxsteps=300000):
  sentinel=0x0100;self.push(sentinel);self.pc=addr;start=self.steps
  while self.pc!=sentinel:
   if self.steps-start>maxsteps:raise RuntimeError(('runaway',hex(self.pc),self.current_page))
   self.step()
 def rel(self,x):return x-256 if x&128 else x
 def step(self):
  if self.pc==GATE:
   page=self.a
   if not 0<=page<len(PAGES):raise RuntimeError(('bad page',page))
   self.load_page(page);self.pc=self.pop();self.steps+=1;return
  p=self.pc;op=self.m[p];self.pc=(p+1)&65535;self.steps+=1
  def imm():v=self.m[self.pc];self.pc=(self.pc+1)&65535;return v
  def word():lo=imm();return lo|(imm()<<8)
  if op==0xCD:self.push(self.pc+2);self.pc=word()
  elif op==0xC3:self.pc=word()
  elif op==0xC9:self.pc=self.pop()
  elif op==0xAF:self.a=0;self.z=True
  elif op==0x32:self.w(word(),self.a)
  elif op==0x3A:self.a=self.m[word()]
  elif op==0x3E:self.a=imm()
  elif op==0x2A:self.sethl(self.rdw(word()))
  elif op==0x22:self.wrw(word(),self.hl())
  elif op==0x21:self.sethl(word())
  elif op==0x11:self.setde(word())
  elif op==0x01:self.setbc(word())
  elif op==0x19:self.sethl((self.hl()+self.de())&65535)
  elif op==0x09:self.sethl((self.hl()+self.bc())&65535)
  elif op==0x29:self.sethl((self.hl()*2)&65535)
  elif op==0x7E:self.a=self.m[self.hl()]
  elif op==0x77:self.w(self.hl(),self.a)
  elif op==0x23:self.sethl((self.hl()+1)&65535)
  elif op==0xB7:self.z=(self.a==0)
  elif op==0xC0:
   if not self.z:self.pc=self.pop()
  elif op==0x20:
   d=self.rel(imm());
   if not self.z:self.pc=(self.pc+d)&65535
  elif op==0x28:
   d=self.rel(imm());
   if self.z:self.pc=(self.pc+d)&65535
  elif op==0x18:self.pc=(self.pc+self.rel(imm()))&65535
  elif op==0x3D:self.a=(self.a-1)&255;self.z=(self.a==0)
  elif op==0xE6:self.a&=imm();self.z=(self.a==0)
  elif op==0xFE:
   n=imm();self.z=(self.a==n)
  elif op==0xE5:self.push(self.hl())
  elif op==0xE1:self.sethl(self.pop())
  elif op==0x6F:self.l=self.a
  elif op==0x26:self.h=imm()
  elif op==0x5E:self.e=self.m[self.hl()]
  elif op==0x56:self.d=self.m[self.hl()]
  elif op==0x7B:self.a=self.e
  elif op==0x7A:self.a=self.d
  elif op==0x06:self.b=imm()
  elif op==0x10:
   d=self.rel(imm());self.b=(self.b-1)&255
   if self.b:self.pc=(self.pc+d)&65535
  elif op==0xD3:
   port=imm();self.ports[port]=self.a
   if port==0xA0:self.psg_sel=self.a
   elif port==0xA1:self.ports[0x100+self.psg_sel]=self.a
  elif op==0xED:
   op2=imm()
   if op2!=0xB0:raise RuntimeError(('ed',hex(op2),hex(p)))
   n=self.bc();src=self.hl();dst=self.de();self.m[dst:dst+n]=self.m[src:src+n];self.sethl(src+n);self.setde(dst+n);self.setbc(0)
  else:raise RuntimeError(('opcode',hex(op),hex(p)))

def ref_init(block):
 s={'mem':bytearray(0xB0),'psg':{7:0x2e,8:0,9:0,10:0},'wait':0,'ended':0}
 s['mem'][0xAF]=0x1f
 stream=struct.unpack_from('<H',block,10)[0];s['ptr']=stream+1;s['wait']=block[stream]
 if s['wait']==0:ref_event(block,s)
 return s
def ref_sil(s):
 for i in range(0xAA,0xB0):s['mem'][i]=0
 for r in (8,9,10):s['psg'][r]=0
def ref_event(b,s):
 p=s['ptr'];fm,vm,wm=b[p:p+3];p+=3
 for i in range(5):
  if wm>>i&1:
   wid=b[p];p+=1;s['mem'][32*i:32*i+32]=b[16+wid*32:16+(wid+1)*32]
 for i in range(5):
  if fm>>i&1:s['mem'][0xA0+2*i:0xA2+2*i]=b[p:p+2];p+=2
 if fm&0x20:s['psg'][0]=b[p];s['psg'][1]=b[p+1];p+=2
 if fm&0x40:s['psg'][6]=b[p];p+=1
 for i in range(5):
  if vm>>i&1:s['mem'][0xAA+i]=b[p]&15;p+=1
 if vm&0x20:s['psg'][8]=b[p]&15;p+=1
 if vm&0x40:s['psg'][9]=b[p]&15;p+=1
 delay=b[p];p+=1
 if delay==255:
  if b[4]&1:
   ref_sil(s);s['ptr']=struct.unpack_from('<H',b,8)[0];s['wait']=0
  else:
   ref_sil(s);s['ended']=1
 else:s['wait']=delay;s['ptr']=p
def ref_play(b,s):
 if s['ended']:return
 if s['wait']:s['wait']-=1
 else:ref_event(b,s)
def compare(cpu,s,i,tick):
 if cpu.m[0xB800:0xB8B0]!=s['mem']:
  for j,(x,y) in enumerate(zip(cpu.m[0xB800:0xB8B0],s['mem'])):
   if x!=y:raise AssertionError((i,tick,'scc',hex(0xB800+j),x,y,cpu.current_page))
 for r,v in s['psg'].items():
  if cpu.ports.get(0x100+r,0)!=v:raise AssertionError((i,tick,'psg',r,cpu.ports.get(0x100+r,0),v,cpu.current_page))
 if cpu.m[WAIT]!=s['wait'] or cpu.m[ENDED]!=s['ended']:
  raise AssertionError((i,tick,'state',cpu.m[WAIT],s['wait'],cpu.m[ENDED],s['ended'],cpu.current_page))

results=[]
for i in range(track_count):
 full=(Path('/mnt/data/bomberman94_ksp_build')/f'{i:02d}.bin').read_bytes()
 cpu=CPU(first_pages[i]);cpu.run_call(INIT);s=ref_init(full);compare(cpu,s,i,-1)
 frames=struct.unpack_from('<H',full,6)[0]
 for tick in range(frames+3):
  cpu.run_call(PLAY);ref_play(full,s);compare(cpu,s,i,tick)
 results.append({'track':i,'frames':frames,'steps':cpu.steps,'ended':cpu.m[ENDED],'loop':bool(full[4]&1),'pages_seen':cpu.switches})
out={'ok':True,'tracks':len(results),'pages':page_count,'total_steps':sum(x['steps'] for x in results),'max_steps':max(x['steps'] for x in results),'results':results}
(BASE/'z80_validation.json').write_text(json.dumps(out,indent=2))
print(json.dumps({k:v for k,v in out.items() if k!='results'},indent=2))
