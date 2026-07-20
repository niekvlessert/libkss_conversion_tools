#!/usr/bin/env python3
from pathlib import Path
import struct, json, hashlib, csv, zipfile
ORIGIN=0x4000
TRACK=0x4800; WAVE=0x4810
PTR=0x4700; WAIT=0x4702; ENDED=0x4703; FM=0x4704; VM=0x4705; WM=0x4706; LOOPPTR=0x4707
SCC_WAVE=0xB800; SCC_FREQ=0xB8A0; SCC_VOL=0xB8AA; SCC_KEY=0xB8AF
class A:
 def __init__(self): self.b=bytearray(); self.labels={}; self.fix=[]
 @property
 def pc(self): return ORIGIN+len(self.b)
 def label(self,n):
  if n in self.labels: raise ValueError(n)
  self.labels[n]=self.pc
 def e(self,*x): self.b.extend(v&255 for v in x)
 def w(self,v): self.e(v,v>>8)
 def absfix(self,op,label): self.e(op); self.fix.append(('abs',len(self.b),label)); self.e(0,0)
 def call(self,l): self.absfix(0xCD,l)
 def jp(self,l,cond=None): self.absfix({None:0xC3,'z':0xCA,'nz':0xC2,'c':0xDA,'nc':0xD2}[cond],l)
 def jr(self,l,cond=None): self.e({None:0x18,'z':0x28,'nz':0x20,'c':0x38,'nc':0x30}[cond]); self.fix.append(('rel',len(self.b),l)); self.e(0)
 def finish(self):
  for typ,pos,l in self.fix:
   target=self.labels[l]
   if typ=='abs': self.b[pos:pos+2]=struct.pack('<H',target)
   else:
    d=target-(ORIGIN+pos+1)
    if not -128<=d<=127: raise ValueError(f'JR {l} out of range {d}')
    self.b[pos]=d&255
  return bytes(self.b)

def lda_nn(a,nn): a.e(0x3A);a.w(nn)
def ldnn_a(a,nn): a.e(0x32);a.w(nn)
def ldhl_nnmem(a,nn): a.e(0x2A);a.w(nn)
def ldnn_hl(a,nn): a.e(0x22);a.w(nn)
def ldhl(a,nn): a.e(0x21);a.w(nn)
def ldde(a,nn): a.e(0x11);a.w(nn)
def ldbc(a,nn): a.e(0x01);a.w(nn)
def out_reg_a(a,reg): a.e(0xD3,reg)
def psg_const(a,reg,val): a.e(0x3E,reg);out_reg_a(a,0xA0);a.e(0x3E,val);out_reg_a(a,0xA1)

def build_engine():
 a=A()
 # INIT
 a.label('init')
 a.call('silence')
 a.e(0xAF);ldnn_a(a,ENDED)
 # Put original Sound Cartridge in SCC-I/SCC+ mode and expose B800-B8AF.
 a.e(0x3E,0x20);ldnn_a(a,0xBFFE)
 a.e(0x3E,0x80);ldnn_a(a,0xB000)
 psg_const(a,7,0x2E) # tone A + noise B
 a.e(0x3E,0x1F);ldnn_a(a,SCC_KEY)
 # loop pointer (first event payload)
 ldhl_nnmem(a,TRACK+8);ldde(a,TRACK);a.e(0x19);ldnn_hl(a,LOOPPTR)
 # stream start, consume first delay
 ldhl_nnmem(a,TRACK+10);ldde(a,TRACK);a.e(0x19)
 a.e(0x7E,0x23);ldnn_a(a,WAIT);ldnn_hl(a,PTR)
 a.e(0xB7);a.jr('init_done','nz');a.call('apply_event')
 a.label('init_done');a.e(0xC9)
 # PLAY
 a.label('play')
 lda_nn(a,ENDED);a.e(0xB7,0xC0) # RET NZ
 lda_nn(a,WAIT);a.e(0xB7);a.jr('apply_event','z')
 a.e(0x3D);ldnn_a(a,WAIT);a.e(0xC9)
 # APPLY EVENT
 a.label('apply_event')
 ldhl_nnmem(a,PTR)
 a.e(0x7E,0x23);ldnn_a(a,FM)
 a.e(0x7E,0x23);ldnn_a(a,VM)
 a.e(0x7E,0x23);ldnn_a(a,WM)
 # waveform IDs first
 for i in range(5):
  lda_nn(a,WM);a.e(0xE6,1<<i);a.jr(f'wave{i}_done','z')
  a.e(0x7E,0x23,0xE5,0x6F,0x26,0)
  for _ in range(5): a.e(0x29)
  ldbc(a,WAVE);a.e(0x09);ldde(a,SCC_WAVE+32*i);ldbc(a,32);a.e(0xED,0xB0,0xE1)
  a.label(f'wave{i}_done')
 # SCC frequencies
 for i in range(5):
  lda_nn(a,FM);a.e(0xE6,1<<i);a.jr(f'freq{i}_done','z')
  a.e(0x7E,0x23);ldnn_a(a,SCC_FREQ+2*i)
  a.e(0x7E,0x23,0xE6,0x0F);ldnn_a(a,SCC_FREQ+2*i+1)
  a.label(f'freq{i}_done')
 # PSG tone A period
 lda_nn(a,FM);a.e(0xE6,0x20);a.jr('psgf_done','z')
 a.e(0x5E,0x23,0x56,0x23)
 a.e(0x3E,0);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
 a.e(0x3E,1);out_reg_a(a,0xA0);a.e(0x7A);out_reg_a(a,0xA1)
 a.label('psgf_done')
 # PSG noise period
 lda_nn(a,FM);a.e(0xE6,0x40);a.jr('noise_f_done','z')
 a.e(0x5E,0x23,0x3E,6);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
 a.label('noise_f_done')
 # SCC volumes
 for i in range(5):
  lda_nn(a,VM);a.e(0xE6,1<<i);a.jr(f'vol{i}_done','z')
  a.e(0x7E,0x23,0xE6,0x0F);ldnn_a(a,SCC_VOL+i)
  a.label(f'vol{i}_done')
 # PSG tone/noise volumes
 lda_nn(a,VM);a.e(0xE6,0x20);a.jr('psgv_done','z')
 a.e(0x5E,0x23,0x3E,8);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
 a.label('psgv_done')
 lda_nn(a,VM);a.e(0xE6,0x40);a.jr('noise_v_done','z')
 a.e(0x5E,0x23,0x3E,9);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
 a.label('noise_v_done')
 # Next delay / end marker
 a.e(0x7E,0x23,0xFE,0xFF);a.jr('stream_end','z')
 ldnn_a(a,WAIT);ldnn_hl(a,PTR);a.e(0xC9)
 a.label('stream_end')
 lda_nn(a,TRACK+4);a.e(0xE6,1);a.jr('final_end','z')
 a.call('silence');ldhl_nnmem(a,LOOPPTR);ldnn_hl(a,PTR);a.e(0xAF);ldnn_a(a,WAIT);a.e(0xC9)
 a.label('final_end')
 a.call('silence');a.e(0x3E,1);ldnn_a(a,ENDED);a.e(0xC9)
 # Silence SCC+ and PSG volumes.
 a.label('silence')
 a.e(0xAF);ldhl(a,SCC_VOL);a.e(0x06,6)
 a.label('sil_loop');a.e(0x77,0x23,0x10);a.fix.append(('rel',len(a.b),'sil_loop'));a.e(0)
 for r in (8,9,10): psg_const(a,r,0)
 a.e(0xC9)
 code=a.finish()
 if len(code)>0x700: raise ValueError(len(code))
 return code,a.labels

def validate_track_block(d):
 assert d[:4]==b'HSC1'; nw=d[5]; frames,loop_payload,stream_off=struct.unpack_from('<HHH',d,6)
 assert 16+32*nw==stream_off and stream_off<len(d)
 waves=[d[16+32*i:16+32*(i+1)] for i in range(nw)]
 p=stream_off; events=0
 # first byte delay, then event masks. Follow linear stream only.
 while True:
  delay=d[p];p+=1
  if delay==0xFF: break
  fm,vm,wm=d[p:p+3];p+=3;events+=1
  for i in range(5):
   if wm>>i&1:
    assert d[p]<nw;p+=1
  p += 2*sum((fm>>i)&1 for i in range(6)) + ((fm>>6)&1)
  p += sum((vm>>i)&1 for i in range(7))
  assert p<=len(d)
 assert p==len(d), (p,len(d))
 assert stream_off < loop_payload < len(d)
 return frames,nw,events

def main():
 build=Path('/mnt/data/parodius_ksp_build'); outdir=Path('/mnt/data/parodius_sccplus_ksp');outdir.mkdir(exist_ok=True)
 blocks=[];meta=[]
 for i in range(38):
  d=(build/f'{i:02d}.bin').read_bytes();frames,nw,events=validate_track_block(d);blocks.append(d)
  m=json.loads((build/f'{i:02d}.json').read_text());m.update({'frames':frames,'validated_events':events});meta.append(m)
 code,labels=build_engine()
 template=bytearray(0x4000);template[:len(code)]=code
 # KSSX prefix: 32-byte header + one harmless byte load image.
 header=bytearray(0x20);header[:4]=b'KSSX';struct.pack_into('<HHHH',header,4,0x4000,1,labels['init'],labels['play'])
 header[0x0C]=0;header[0x0D]=0;header[0x0E]=0;header[0x0F]=0
 kcp=bytearray(b'KCPX')+bytes((1,len(blocks),len(blocks),0))+struct.pack('<HHHH',0x4000,0x4000,TRACK,0)
 songmap=bytearray([0xFF])*256;pagemap=bytearray([0xFF])*256
 for i in range(len(blocks)): songmap[i]=i;pagemap[i]=i
 kcp += songmap+pagemap
 assert len(kcp)==0x210
 data=bytearray(header)+b'\xC9'+kcp+template
 assert len(data)==0x4231
 for block in blocks:
  rec=struct.pack('<HH',TRACK-0x4000,len(block))+block
  data += struct.pack('<H',len(rec))+rec
 out=outdir/'Parodius_Da_PC_Engine_SCCplus_prototype.ksp';out.write_bytes(data)
 # Companion reports
 info={
  'status':'prototype; structurally KCPX-compatible, not yet audio-validated on real MSX/openMSX',
  'source_hes_sha256':hashlib.sha256(Path('/mnt/data/parodius_hes/KM92003.hes').read_bytes()).hexdigest(),
  'output_sha256':hashlib.sha256(data).hexdigest(),'output_size':len(data),'track_count':len(blocks),
  'engine':{'load':'0x4000','init':hex(labels['init']),'play':hex(labels['play']),'bytes':len(code),'track_data':'0x4800'},
  'conversion':{'HuC6280_channels_1_5':'SCC+ channels 1-5','HuC6280_channel_6':'MSX PSG tone A','HuC6280_DDA_and_noise':'MSX PSG noise B amplitude envelope','stereo':'folded to mono','timing':'60 Hz state/event replay','loops':'whole traced duration; original intro/loop split not retained'},
  'known_player_issue':'Current KSPPLAYER kcpx_silence clears 0x9880 SCC registers but not SCC+ 0xB8AA-0xB8AF; add the included source patch to guarantee silence on exit.',
  'tracks':meta
 }
 (outdir/'Parodius_Da_PC_Engine_SCCplus_prototype.json').write_text(json.dumps(info,indent=2,ensure_ascii=False))
 readme=f'''# Parodius Da! PC Engine to SCC+ KSP prototype\n\nThis is an experimental KCPX/KSP file for the current `KSPPLAYER.COM` complete-page path.\n\n- Tracks: 38\n- Z80 replay engine: {len(code)} bytes at 4000H\n- Track overlay: 4800H onward\n- Output size: {len(data)} bytes\n- SHA-256: {hashlib.sha256(data).hexdigest()}\n\n## Mapping\n\n- HuC6280 voices 1-5 -> the five independent SCC+ voices.\n- HuC6280 voice 6 -> MSX PSG tone A.\n- HuC6280 DDA/sample percussion and PSG noise -> an approximate PSG noise-B amplitude envelope.\n- Stereo balance is folded to mono.\n- Playback state is sampled at 60 Hz.\n\n## Hardware/runtime\n\nThe engine writes 20H to BFFEH and 80H to B000H, then uses the SCC+ register block at B800H-B8AFH. Use an original Snatcher/SD Snatcher Sound Cartridge or compatible SCC+ hardware in the SCC slot selected by KSPPLAYER.\n\n## Important status\n\nThis is a real generated replay container, not a renamed HES file. Its KCPX structure, all 38 overlays, event streams, waveform IDs, bounds and hashes were validated host-side. It has not been listened to in openMSX or on real hardware in this environment. DDA percussion is intentionally approximate.\n\nThe current KSPPLAYER source should also clear B8AAH-B8AFH in `kcpx_silence`; see `kspplayer_sccplus_silence.patch`.\n'''
 (outdir/'README.md').write_text(readme)
 patch='''--- a/msx/KSPPLAYER.asm\n+++ b/msx/KSPPLAYER.asm\n@@\n kcpx_silence:\n@@\n         ld      a,0x3F\n         ld      (0x9000),a\n+        ; Silence SCC-I/SCC+ as well. A KCP engine can leave the Sound\n+        ; Cartridge in SCC+ mode, where the 9800H compatibility window is\n+        ; not the active register block.\n+        xor     a\n+        ld      hl,0xB8AA\n+        ld      b,6\n+kcpx_silence_sccplus:\n+        ld      (hl),a\n+        inc     hl\n+        djnz    kcpx_silence_sccplus\n         xor     a\n         ld      hl,0x9880\n'''
 (outdir/'kspplayer_sccplus_silence.patch').write_text(patch)
 # engine/map and track CSV
 (outdir/'engine.bin').write_bytes(code)
 (outdir/'engine_map.json').write_text(json.dumps({'origin':hex(ORIGIN),'size':len(code),'labels':{k:hex(v) for k,v in labels.items()}},indent=2))
 with (outdir/'tracks.csv').open('w',newline='',encoding='utf-8') as f:
  w=csv.DictWriter(f,fieldnames=['index','id','title','duration','loop','block_bytes','waves','events','dda_writes']);w.writeheader()
  for i,m in enumerate(meta):w.writerow({'index':i,'id':f"0x{m['id']:02X}",'title':m['title'],'duration':m['duration'],'loop':m['loop'],'block_bytes':m['block_bytes'],'waves':m['waves'],'events':m['events'],'dda_writes':m['dda_writes']})
 zip_path=outdir/'Parodius_Da_PC_Engine_SCCplus_prototype_bundle.zip'
 with zipfile.ZipFile(zip_path,'w',zipfile.ZIP_DEFLATED) as z:
  for p in outdir.iterdir():
   if p!=zip_path:z.write(p,p.name)
 print(json.dumps({'ksp':str(out),'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'engine_size':len(code),'init':hex(labels['init']),'play':hex(labels['play']),'bundle':str(zip_path)},indent=2))
if __name__=='__main__':main()
