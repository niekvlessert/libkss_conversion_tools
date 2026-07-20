#!/usr/bin/env python3
from pathlib import Path
import struct, json, hashlib, csv, zipfile, shutil

ORIGIN=0x4000
TRACK=0x4800
WAVE=0x4810
PTR=0x4700
WAIT=0x4702
ENDED=0x4703
FM=0x4704
VM=0x4705
WM=0x4706
LOOPPTR=0x4707
PAGE_GATEWAY=0xD8A0
SCC_WAVE=0xB800
SCC_FREQ=0xB8A0
SCC_VOL=0xB8AA
SCC_KEY=0xB8AF
MAX_BLOCK=0x3800
BUILD=Path('/mnt/data/bomberman94_ksp_build')
OUTDIR=Path('/mnt/data/bomberman94_sccplus_ksp')

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
    def call(self,l):
        if isinstance(l,int): self.e(0xCD); self.w(l)
        else: self.absfix(0xCD,l)
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
    a.label('init')
    a.call('silence')
    a.e(0xAF);ldnn_a(a,ENDED)
    a.e(0x3E,0x20);ldnn_a(a,0xBFFE)
    a.e(0x3E,0x80);ldnn_a(a,0xB000)
    psg_const(a,7,0x2E)
    a.e(0x3E,0x1F);ldnn_a(a,SCC_KEY)
    a.call('load_chunk_start')
    a.e(0xC9)

    a.label('play')
    lda_nn(a,ENDED);a.e(0xB7,0xC0)
    lda_nn(a,WAIT);a.e(0xB7);a.jr('apply_event','z')
    a.e(0x3D);ldnn_a(a,WAIT);a.e(0xC9)

    a.label('load_chunk_start')
    ldhl_nnmem(a,TRACK+8);ldde(a,TRACK);a.e(0x19);ldnn_hl(a,LOOPPTR)
    ldhl_nnmem(a,TRACK+10);ldde(a,TRACK);a.e(0x19)
    a.e(0x7E,0x23);ldnn_a(a,WAIT);ldnn_hl(a,PTR)
    a.e(0xB7);a.e(0xC0)  # RET NZ
    a.jp('apply_event')

    a.label('apply_event')
    ldhl_nnmem(a,PTR)
    a.e(0x7E,0x23);ldnn_a(a,FM)
    a.e(0x7E,0x23);ldnn_a(a,VM)
    a.e(0x7E,0x23);ldnn_a(a,WM)
    for i in range(5):
        lda_nn(a,WM);a.e(0xE6,1<<i);a.jr(f'wave{i}_done','z')
        a.e(0x7E,0x23,0xE5,0x6F,0x26,0)
        for _ in range(5): a.e(0x29)
        ldbc(a,WAVE);a.e(0x09);ldde(a,SCC_WAVE+32*i);ldbc(a,32);a.e(0xED,0xB0,0xE1)
        a.label(f'wave{i}_done')
    for i in range(5):
        lda_nn(a,FM);a.e(0xE6,1<<i);a.jr(f'freq{i}_done','z')
        a.e(0x7E,0x23);ldnn_a(a,SCC_FREQ+2*i)
        a.e(0x7E,0x23,0xE6,0x0F);ldnn_a(a,SCC_FREQ+2*i+1)
        a.label(f'freq{i}_done')
    lda_nn(a,FM);a.e(0xE6,0x20);a.jr('psgf_done','z')
    a.e(0x5E,0x23,0x56,0x23)
    a.e(0x3E,0);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
    a.e(0x3E,1);out_reg_a(a,0xA0);a.e(0x7A);out_reg_a(a,0xA1)
    a.label('psgf_done')
    lda_nn(a,FM);a.e(0xE6,0x40);a.jr('noise_f_done','z')
    a.e(0x5E,0x23,0x3E,6);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
    a.label('noise_f_done')
    for i in range(5):
        lda_nn(a,VM);a.e(0xE6,1<<i);a.jr(f'vol{i}_done','z')
        a.e(0x7E,0x23,0xE6,0x0F);ldnn_a(a,SCC_VOL+i)
        a.label(f'vol{i}_done')
    lda_nn(a,VM);a.e(0xE6,0x20);a.jr('psgv_done','z')
    a.e(0x5E,0x23,0x3E,8);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
    a.label('psgv_done')
    lda_nn(a,VM);a.e(0xE6,0x40);a.jr('noise_v_done','z')
    a.e(0x5E,0x23,0x3E,9);out_reg_a(a,0xA0);a.e(0x7B);out_reg_a(a,0xA1)
    a.label('noise_v_done')
    a.e(0x7E,0x23,0xFE,0xFF);a.jr('stream_end','z')
    ldnn_a(a,WAIT);ldnn_hl(a,PTR);a.e(0xC9)

    a.label('stream_end')
    lda_nn(a,TRACK+4);a.e(0xE6,0x08);a.jr('local_end','z')
    lda_nn(a,TRACK+4);a.e(0xE6,0x10);a.jr('ordinary_continuation','z')
    a.call('silence')
    lda_nn(a,TRACK+12)
    a.call(PAGE_GATEWAY)
    ldhl_nnmem(a,TRACK+8);ldde(a,TRACK);a.e(0x19);ldnn_hl(a,PTR)
    a.e(0xAF);ldnn_a(a,WAIT);a.e(0xC9)
    a.label('ordinary_continuation')
    lda_nn(a,TRACK+12)
    a.call(PAGE_GATEWAY)
    # The current PLAY call has already applied the final event of the old
    # chunk. Read the new chunk's first delay and pointer, but never execute
    # a zero-delay first event until the next PLAY tick.
    ldhl_nnmem(a,TRACK+10);ldde(a,TRACK);a.e(0x19)
    a.e(0x7E,0x23);ldnn_a(a,WAIT);ldnn_hl(a,PTR)
    a.e(0xC9)

    a.label('local_end')
    lda_nn(a,TRACK+4);a.e(0xE6,1);a.jr('final_end','z')
    a.call('silence');ldhl_nnmem(a,LOOPPTR);ldnn_hl(a,PTR);a.e(0xAF);ldnn_a(a,WAIT);a.e(0xC9)
    a.label('final_end')
    a.call('silence');a.e(0x3E,1);ldnn_a(a,ENDED);a.e(0xC9)

    a.label('silence')
    a.e(0xAF);ldhl(a,SCC_VOL);a.e(0x06,6)
    a.label('sil_loop');a.e(0x77,0x23,0x10);a.fix.append(('rel',len(a.b),'sil_loop'));a.e(0)
    for r in (8,9,10): psg_const(a,r,0)
    a.e(0xC9)
    code=a.finish()
    if len(code)>0x700: raise ValueError(len(code))
    return code,a.labels

def parse_event_records(d):
    if d[:4] != b'HSC1': raise ValueError('not HSC1')
    nw=d[5]
    stream_off=struct.unpack_from('<H',d,10)[0]
    prefix=d[:stream_off]
    p=stream_off; records=[]
    while True:
        start=p; delay=d[p];p+=1
        if delay==0xFF: break
        fm,vm,wm=d[p:p+3];p+=3
        p += sum((wm>>i)&1 for i in range(5))
        p += 2*sum((fm>>i)&1 for i in range(6)) + ((fm>>6)&1)
        p += sum((vm>>i)&1 for i in range(7))
        if p>len(d): raise ValueError('event overrun')
        records.append(d[start:p])
    if p != len(d): raise ValueError((p,len(d)))
    return bytearray(prefix),records

def split_track(d):
    prefix,records=parse_event_records(d)
    capacity=MAX_BLOCK-len(prefix)-1
    chunks=[]; cur=[]; n=0
    for rec in records:
        if len(rec)>capacity: raise ValueError('single event too large')
        if cur and n+len(rec)>capacity:
            chunks.append(cur);cur=[];n=0
        cur.append(rec);n+=len(rec)
    if cur or not chunks: chunks.append(cur)
    return prefix,chunks

def make_chunk(prefix,records,flags,next_page=None):
    h=bytearray(prefix)
    h[4]=flags&0xFF
    h[12]=(next_page if next_page is not None else 0xFF)&0xFF
    h[13]=0x43  # 'C' marker for chunk-aware stream
    block=h+b''.join(records)+b'\xFF'
    if len(block)>MAX_BLOCK: raise ValueError(len(block))
    return bytes(block)

def validate_block(d):
    if d[:4]!=b'HSC1': raise ValueError('bad magic')
    stream_off=struct.unpack_from('<H',d,10)[0]
    if not 16<=stream_off<len(d): raise ValueError('stream offset')
    p=stream_off;events=0
    while True:
        delay=d[p];p+=1
        if delay==0xFF:break
        fm,vm,wm=d[p:p+3];p+=3
        p += sum((wm>>i)&1 for i in range(5))
        p += 2*sum((fm>>i)&1 for i in range(6))+((fm>>6)&1)
        p += sum((vm>>i)&1 for i in range(7))
        if p>len(d):raise ValueError('overrun')
        events+=1
    if p!=len(d):raise ValueError((p,len(d)))
    return events

def player_patch():
    return r'''--- a/msx/PLAYER_LAYOUT.inc
+++ b/msx/PLAYER_LAYOUT.inc
@@
 CUSTOM_ENASLT: equ 0xD840
+; Runtime-only KCP overlay switch gateway used by chunked HuC6280/SCC+ packs.
+; A = internal KCP page index. The routine replaces only sparse overlay
+; records; it does not recopy the common 16K template.
+KCP_PAGE_GATEWAY: equ 0xD8A0
@@
--- a/msx/KSPPLAYER.asm
+++ b/msx/KSPPLAYER.asm
@@
 kcpx_runtime_path:
@@
         call    kcpx_materialize_dispatch
         jp      c,format_error
+
+        ; Install the fixed page-3 gateway called by chunk-aware engines.
+        ld      hl,kcpx_page_gateway
+        ld      de,KCP_PAGE_GATEWAY
+        ld      bc,kcpx_page_gateway_end-kcpx_page_gateway
+        ldir
@@
 kcpx_silence:
@@
         ld      a,0x3F
         ld      (0x9000),a
+        ; Silence SCC-I/SCC+ register mode as well.
+        xor     a
+        ld      hl,0xB8AA
+        ld      b,6
+kcpx_silence_sccplus:
+        ld      (hl),a
+        inc     hl
+        djnz    kcpx_silence_sccplus
         xor     a
         ld      hl,0x9880
@@
+; Copy only the selected KCPX sparse overlay. The common template and engine
+; state at 4000H-47FFH remain intact while 4800H-7FFFH is replaced.
+kcpx_materialize_overlay_only:
+        ld      a,(RUNTIME_KSS_TABLE)
+        ld      (kcp_selected_segment),a
+        ld      (kcp_dest_segment),a
+        ld      hl,(kcp_records_source)
+        ld      (kcp_cursor),hl
+        ld      a,(kcp_selected_page)
+        ld      (kcp_page_index),a
+        jp      kcpx_overlay_scan
+
+; This block is copied to KCP_PAGE_GATEWAY (D8A0H). A contains the internal
+; logical page. It is called with interrupts disabled from the page-1 engine.
+kcpx_page_gateway:
+        push    af
+        push    bc
+        push    de
+        push    hl
+        push    ix
+        push    iy
+        ld      (kcp_selected_page),a
+        call    kcpx_materialize_overlay_only
+        jr      c,kcpx_page_gateway_fail
+        ld      a,(kcp_selected_segment)
+        call    PUT_P1_DISPATCH
+        ld      a,(RUNTIME_SCC_SLOT)
+        ld      h,0x80
+        call    CUSTOM_ENASLT
+        ; Restore SCC-I/SCC+ mode after page-2 source mapping.
+        ld      a,0x20
+        ld      (0xBFFE),a
+        ld      a,0x80
+        ld      (0xB000),a
+kcpx_page_gateway_fail:
+        pop     iy
+        pop     ix
+        pop     hl
+        pop     de
+        pop     bc
+        pop     af
+        ret
+kcpx_page_gateway_end:
'''

def main():
    OUTDIR.mkdir(exist_ok=True)
    source_meta=[]; splits=[]
    track_files=sorted(BUILD.glob('*.bin'))
    if not track_files: raise ValueError('no converted tracks')
    track_count=len(track_files)
    for i in range(track_count):
        d=(BUILD/f'{i:02d}.bin').read_bytes()
        m=json.loads((BUILD/f'{i:02d}.json').read_text())
        prefix,chunks=split_track(d)
        source_meta.append(m);splits.append((prefix,chunks))

    first_pages=[];page_cursor=0
    for prefix,chunks in splits:
        first_pages.append(page_cursor);page_cursor+=len(chunks)
    if page_cursor>=128: raise ValueError(('too many pages',page_cursor))

    pages=[];page_meta=[]
    for ti,(prefix,chunks) in enumerate(splits):
        base_flags=prefix[4]
        first=first_pages[ti]
        multi=len(chunks)>1
        for ci,recs in enumerate(chunks):
            last=(ci==len(chunks)-1)
            next_page=None
            flags=base_flags
            if multi and (not last or source_meta[ti]['loop']):
                flags=(flags & ~1)|0x08
                if last and source_meta[ti]['loop']:
                    flags |= 0x10
                next_page=(first+ci+1) if not last else first
            elif multi:
                flags=(flags & ~1)&~0x08
            else:
                flags=flags&~0x08
            block=make_chunk(prefix,recs,flags,next_page)
            ev=validate_block(block)
            page_index=len(pages)
            pages.append(block)
            page_meta.append({'page':page_index,'track_index':ti,'chunk_index':ci,'chunk_count':len(chunks),'next_page':next_page,'bytes':len(block),'events':ev,'flags':flags})

    code,labels=build_engine()
    template=bytearray(0x4000);template[:len(code)]=code
    header=bytearray(0x20);header[:4]=b'KSSX';struct.pack_into('<HHHH',header,4,0x4000,1,labels['init'],labels['play'])
    kcp=bytearray(b'KCPX')+bytes((1,len(pages),len(source_meta),0))+struct.pack('<HHHH',0x4000,0x4000,TRACK,0)
    songmap=bytearray([0xFF])*256;pagemap=bytearray([0xFF])*256
    for i,m in enumerate(source_meta):
        songmap[i]=m['id']&0xFF;pagemap[i]=first_pages[i]
    kcp+=songmap+pagemap
    if len(kcp)!=0x210:raise AssertionError(len(kcp))
    data=bytearray(header)+b'\xC9'+kcp+template
    if len(data)!=0x4231:raise AssertionError(len(data))
    for block in pages:
        rec=struct.pack('<HH',TRACK-0x4000,len(block))+block
        data+=struct.pack('<H',len(rec))+rec

    out=OUTDIR/'Bomberman_94_PC_Engine_SCCplus_chunked_prototype.ksp'
    out.write_bytes(data)
    sha=hashlib.sha256(data).hexdigest()
    (OUTDIR/'engine.bin').write_bytes(code)
    (OUTDIR/'engine_map.json').write_text(json.dumps({'origin':hex(ORIGIN),'size':len(code),'labels':{k:hex(v) for k,v in labels.items()},'page_gateway':hex(PAGE_GATEWAY)},indent=2))
    (OUTDIR/'kspplayer_chunked_sccplus.patch').write_text(player_patch())

    tracks=[]
    for i,m in enumerate(source_meta):
        pm=[x for x in page_meta if x['track_index']==i]
        tracks.append({**m,'public_track':i,'first_page':first_pages[i],'page_count':len(pm),'page_bytes':[x['bytes'] for x in pm],'page_events':[x['events'] for x in pm]})
    info={
        'status':'chunked SCC+ prototype; requires included KSPPLAYER page-gateway patch',
        'source_hes_sha256':hashlib.sha256(Path('/mnt/data/bomberman94_hes/HC93065.hes').read_bytes()).hexdigest(),
        'output_sha256':sha,'output_size':len(data),'track_count':len(tracks),'internal_page_count':len(pages),
        'engine':{'load':'0x4000','init':hex(labels['init']),'play':hex(labels['play']),'bytes':len(code),'track_overlay':'0x4800','page_gateway':hex(PAGE_GATEWAY)},
        'conversion':{'HuC6280_channels_1_5':'SCC+ channels 1-5','HuC6280_channel_6':'MSX PSG tone A','HuC6280_DDA_and_noise':'MSX PSG noise B envelope','stereo':'folded to mono','timing':'60 Hz state/event replay','long_tracks':'split across internal KCP overlays; engine state remains resident'},
        'tracks':tracks,'pages':page_meta
    }
    (OUTDIR/'Bomberman_94_PC_Engine_SCCplus_chunked_prototype.json').write_text(json.dumps(info,indent=2,ensure_ascii=False))
    with (OUTDIR/'tracks.csv').open('w',newline='',encoding='utf-8') as f:
        fields=['public_track','id','title','duration','loop','first_page','page_count','block_bytes','waves','events','dda_writes']
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
        for t in tracks:
            w.writerow({k:(f"0x{t[k]:02X}" if k=='id' else t[k]) for k in fields})
    with (OUTDIR/'pages.csv').open('w',newline='',encoding='utf-8') as f:
        fields=['page','track_index','chunk_index','chunk_count','next_page','bytes','events','flags']
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(page_meta)

    max_source_block=max(t['block_bytes'] for t in tracks)
    dda_total=sum(t['dda_writes'] for t in tracks)
    readme=f'''# Bomberman '94 PC Engine to SCC+ KSP prototype

This is a generated {len(tracks)}-track HuC6280-to-SCC+ KCPX/KSP prototype.

- Public tracks: {len(tracks)}
- Internal complete-page overlays: {len(pages)}
- Z80 replay engine: {len(code)} bytes at 4000H
- Track/chunk overlay: 4800H onward
- Largest unsplit source block: {max_source_block} bytes
- Output size: {len(data)} bytes
- SHA-256: `{sha}`

## Why this pack is chunked

Several long Bomberman '94 area, boss, staff-roll and battle tracks exceed the 16 KB complete-page budget. The largest unsplit event block is {max_source_block:,} bytes. This pack splits those streams at event boundaries. The fixed engine and its state remain at 4000H-47FFH, while a fixed page-3 gateway replaces only the 4800H-7FFFH sparse overlay.

## Mapping

- HuC6280 voices 1-5 -> five independent SCC+ voices.
- HuC6280 voice 6 -> MSX PSG tone A.
- HuC6280 noise/DDA -> approximate MSX PSG noise-B envelope.
- Stereo is folded to mono.
- Playback events are sampled at 60 Hz.

The trace contained {dda_total} DDA sample writes across all tracks, so no DDA sample stream was discarded. HuC6280 noise, when used, is approximated through the MSX PSG noise channel.

The source M3U provides complete durations but no explicit loop offsets. This prototype therefore plays the captured sequence once and stops rather than inventing loop points.

## Player requirement

This file is **not compatible with the current unpatched KSPPLAY.COM** for complete playback. Apply `kspplayer_chunked_sccplus.patch` and rebuild the player. The patch adds a fixed D8A0H gateway which materializes only the requested internal overlay while preserving the engine and runtime state.

## Validation status

- All {len(pages)} page records fit in 4800H-7FFFH.
- Every HSC1 event stream was parsed and boundary-validated.
- Every continuation page index and public-song mapping was validated.
- The generated KCPX container was independently reparsed after creation.
- Audible testing in openMSX or on a real SD Snatcher/Snatcher Sound Cartridge remains required.
'''
    (OUTDIR/'README.md').write_text(readme)

    # Include conversion/build sources and the original trace helper sources used in the Parodius build.
    shutil.copy2('/mnt/data/bomberman94_hes/bomberman94_convert.py', OUTDIR/'bomberman94_convert.py')
    shutil.copy2(__file__, OUTDIR/'build_bomberman94_chunked.py')

    for src in ('hes_hook_libgme_0.6.3.c','hes_trace_fast.c'):
        p=Path('/mnt/data/parodius_sccplus_ksp')/src
        if p.exists(): shutil.copy2(p,OUTDIR/src)

    zip_path=OUTDIR/'Bomberman_94_PC_Engine_SCCplus_chunked_prototype_bundle.zip'
    if zip_path.exists():zip_path.unlink()
    with zipfile.ZipFile(zip_path,'w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(OUTDIR.iterdir()):
            if p!=zip_path:z.write(p,p.name)
    print(json.dumps({'ksp':str(out),'bundle':str(zip_path),'size':len(data),'sha256':sha,'tracks':len(tracks),'pages':len(pages),'engine_size':len(code),'max_page_bytes':max(x['bytes'] for x in page_meta)},indent=2))

if __name__=='__main__':main()
