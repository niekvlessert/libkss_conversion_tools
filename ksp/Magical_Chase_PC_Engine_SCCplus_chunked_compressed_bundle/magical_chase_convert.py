#!/usr/bin/env python3
import struct, math, collections, json, os, subprocess, pathlib, re, argparse
LOG=[0.0,0.005524,0.006570,0.007813,0.009291,0.011049,0.013139,0.015625,0.018581,0.022097,0.026278,0.031250,0.037163,0.044194,0.052556,0.062500,0.074325,0.088388,0.105112,0.125000,0.148651,0.176777,0.210224,0.250000,0.297302,0.353553,0.420448,0.500000,0.594604,0.707107,0.840896,1.0]
class Ch:
    def __init__(self):
        self.period=0; self.control=0x40; self.balance=0xFF
        self.wave=[0]*32; self.phase=0; self.noise=0; self.dac=0

def mono_vol(c, master):
    vol=(c.control&31)-60
    li=max(0,min(31,vol+((c.balance>>3)&0x1e)+((master>>3)&0x1e)))
    ri=max(0,min(31,vol+((c.balance<<1)&0x1e)+((master<<1)&0x1e)))
    return max(0,min(15,round((LOG[li]+LOG[ri])*7.5)))

def parse_trace(path, frame_count):
    byframe=[[] for _ in range(frame_count)]
    with open(path,'rb') as f:
        while d:=f.read(16):
            typ,res,addr,frame,time,val=struct.unpack('<BBHII B3x',d)
            if typ==1 and frame<frame_count: byframe[frame].append((time,addr,val))
    ch=[Ch() for _ in range(6)]; latch=0; master=0xff
    states=[]; stats=collections.Counter()
    for wrs in byframe:
        dda_samples=[[] for _ in range(6)]; noise_channels=[]
        for time,addr,val in wrs:
            if addr==0x800: latch=val&7
            elif addr==0x801: master=val
            elif latch<6:
                c=ch[latch]
                if addr==0x802: c.period=(c.period&0xf00)|val
                elif addr==0x803: c.period=(c.period&0x0ff)|((val&15)<<8)
                elif addr==0x804:
                    if (c.control&0x40) and not(val&0x40): c.phase=0
                    c.control=val
                elif addr==0x805: c.balance=val
                elif addr==0x806:
                    val &= 31
                    if not(c.control&0x40):
                        c.wave[c.phase]=val; c.phase=(c.phase+1)&31
                    elif c.control&0x80:
                        c.dac=val; dda_samples[latch].append(val); stats['dda_writes']+=1
                elif addr==0x807 and latch>=4:
                    c.noise=val
                    if val&0x80: noise_channels.append(latch)
        periods=[]; vols=[]; waves=[]
        for i in range(5):
            c=ch[i]; enabled=bool(c.control&0x80); dda=bool(c.control&0x40)
            noise=i>=4 and bool(c.noise&0x80)
            v=mono_vol(c,master) if enabled and not dda and not noise and c.period>9 else 0
            periods.append(max(0,min(4095,c.period-1))) # SCC period+1 = HuC period
            vols.append(v)
            waves.append(tuple(((x-16)*8)&0xff for x in c.wave))
        c=ch[5]
        psg_period=max(1,min(4095,c.period*2))
        psg_vol=mono_vol(c,master) if (c.control&0x80) and not(c.control&0x40) and not(c.noise&0x80) and c.period else 0
        energy=0.0
        for i,samps in enumerate(dda_samples):
            if samps:
                diffs=[abs(samps[j]-samps[j-1]) for j in range(1,len(samps))]
                amp=(sum(diffs)/len(diffs) if diffs else abs(samps[0]-16))/16.0
                cv=max(mono_vol(ch[i],master)/15.0,0.35)
                energy=max(energy,amp*cv)
        for i in noise_channels:
            energy=max(energy,mono_vol(ch[i],master)/15.0)
        noise_vol=max(0,min(15,round(energy*15)))
        states.append((tuple(periods),tuple(vols),tuple(waves),psg_period,psg_vol,8,noise_vol))
    return states,stats

def encode(states, do_loop):
    wave_ids={}; waves=[]
    for st in states:
        for i,w in enumerate(st[2]):
            if st[1][i] and w not in wave_ids:
                wave_ids[w]=len(waves); waves.append(w)
    af=[0]*5; av=[0]*5; aw=[None]*5; ap=1; apv=0; anp=8; anv=0
    events=[]
    for f,st in enumerate(states):
        fm=vm=wm=0
        for i in range(5):
            active=st[1][i]>0
            if active and (st[0][i]!=af[i] or av[i]==0): fm|=1<<i
            if st[1][i]!=av[i]: vm|=1<<i
            if active and st[2][i]!=aw[i]: wm|=1<<i
        if st[4]>0 and (st[3]!=ap or apv==0): fm|=1<<5
        if st[4]!=apv: vm|=1<<5
        if st[6]>0 and (st[5]!=anp or anv==0): fm|=1<<6
        if st[6]!=anv: vm|=1<<6
        if fm or vm or wm or f==0:
            events.append((f,fm,vm,wm,st))
            for i in range(5):
                if fm>>i&1: af[i]=st[0][i]
                if vm>>i&1: av[i]=st[1][i]
                if wm>>i&1: aw[i]=st[2][i]
            if fm>>5&1: ap=st[3]
            if vm>>5&1: apv=st[4]
            if fm>>6&1: anp=st[5]
            if vm>>6&1: anv=st[6]
    expanded=[]; last=-1
    for e in events:
        f=e[0]
        while f-last-1>254:
            last+=255; expanded.append((last,0,0,0,None))
        expanded.append(e); last=f
    stream=bytearray(); first_payload=1; prevf=-1
    for f,fm,vm,wm,st in expanded:
        stream.append(f-prevf-1); stream += bytes((fm,vm,wm))
        if st is not None:
            # wave IDs first so the engine can install instruments before volume changes
            for i in range(5):
                if wm>>i&1: stream.append(wave_ids[st[2][i]])
            for i in range(5):
                if fm>>i&1: stream += struct.pack('<H',st[0][i])
            if fm>>5&1: stream += struct.pack('<H',st[3])
            if fm>>6&1: stream.append(st[5]&31)
            for i in range(5):
                if vm>>i&1: stream.append(st[1][i]&15)
            if vm>>5&1: stream.append(st[4]&15)
            if vm>>6&1: stream.append(st[6]&15)
        prevf=f
    stream.append(0xff)
    header_size=16; stream_off=header_size+32*len(waves)
    flags=(1 if do_loop else 0)|2|4 # full-track loop, PSG ch6 fallback, DDA->noise approximation
    block=bytearray(b'HSC1')+bytes((flags,len(waves)))+struct.pack('<HHH',len(states),stream_off+first_payload,stream_off)+b'\0\0\0\0'
    assert len(block)==16
    for w in waves: block += bytes(w)
    block += stream
    return bytes(block), {'waves':len(waves),'stream_bytes':len(stream),'events':len(events),'block_bytes':len(block)}

def read_tracks(m3u):
    def sec(s):
        if not s:return 0
        if ':' in s:
            a,b=s.split(':');return int(a)*60+int(b)
        return int(s)
    r=[]
    for line in open(m3u,encoding='utf-8-sig'):
        line=line.strip()
        if not line or line.startswith('#'):continue
        p=line.split(','); lf=p[4] if len(p)>4 else ''
        r.append({'id':int(p[1].lstrip('$'),16),'title':p[2],'duration':sec(p[3]),'loop':bool(lf)})
    return r

def convert_batch(start,end,outdir):
    hes='/mnt/data/magical_chase_hes/PL91001.hes';m3u='/mnt/data/magical_chase_hes/PL91001.m3u'; tracks=read_tracks(m3u)
    outdir=pathlib.Path(outdir);outdir.mkdir(parents=True,exist_ok=True)
    for i in range(start,min(end,len(tracks))):
        t=tracks[i]; trace=f'/tmp/magical_chase_{i}.trace'
        env=os.environ.copy();env['HES_TRACE_FILE']=trace;env['LD_PRELOAD']='/tmp/libhestrace.so'
        subprocess.run(['/tmp/hes_trace_fast',hes,m3u,str(i),str(t['duration'])],env=env,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        states,stats=parse_trace(trace,t['duration']*60); os.unlink(trace)
        block,info=encode(states,t['loop'])
        (outdir/f'{i:02d}.bin').write_bytes(block)
        rec={**t,**info,'dda_writes':stats['dda_writes']}
        (outdir/f'{i:02d}.json').write_text(json.dumps(rec,indent=2))
        print(i,rec,flush=True)
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('start',type=int);ap.add_argument('end',type=int);ap.add_argument('outdir');a=ap.parse_args();convert_batch(a.start,a.end,a.outdir)
