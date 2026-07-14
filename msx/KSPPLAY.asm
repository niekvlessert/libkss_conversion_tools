; BASIC-launched MoonSound KSP test player.
;
; The two materialized KSS windows are ZX0-compressed independently by the
; host build.  This complete BIN remains below C000H, so Disk BASIC can finish
; BLOAD before any upper-RAM loader code or stack is overwritten.

        org     0x8000
        jp      start

engine_packed:
        incbin  '@@ENGINE_ZX0@@'
engine_packed_end:
song_packed:
        incbin  '@@SONG_ZX0@@'
song_packed_end:

BIOS_ENASLT:   equ     0x0024
BIOS_RAMAD1:   equ     0xF342
KSS_INIT:      equ     0x7F30
KSS_PLAY:      equ     0x45E5
ZX0_DECODER:   equ     0xBF00
STACK_TOP:     equ     0xF300
SONG_SCRATCH:  equ     0xC000

start:
        im      1
        di
        ld      sp,STACK_TOP

        ld      hl,msg_loading_engine
        ld      de,0x00A0
        call    vram_print
        ld      a,(BIOS_RAMAD1)
        ld      h,0x40
        call    BIOS_ENASLT
        ld      hl,engine_packed
        ld      de,0x4000
        call    ZX0_DECODER

        ld      hl,msg_engine_loaded
        ld      de,0x00C8
        call    vram_print
        ; The packed SONG source is in page 2, as is its 8000H destination.
        ; Move the packed stream into page-3 scratch RAM before decoding.
        ld      hl,song_packed
        ld      de,SONG_SCRATCH
        ld      bc,song_packed_end-song_packed
        ldir
        ld      hl,SONG_SCRATCH
        ld      de,0x8000
        call    ZX0_DECODER

        ld      hl,msg_song_loaded
        ld      de,0x00F0
        call    vram_print

        ; KSS compatibility contract.  The embedded driver uses these two
        ; helpers to discover/select slots while processing its logical MWM
        ; banks.  libkss supplies RSLREG=0 and makes ENASLT a no-op, because
        ; the complete song is already flat at 8000H.  On a real MSX the
        ; original helpers would map a different slot over that song window.
        ld      a,0xC9             ; RET: do not JP BIOS ENASLT from 7CA3H
        ld      (0x7CA3),a
        ld      hl,rslreg_zero_stub
        ld      de,0x7CA8
        ld      bc,rslreg_zero_stub_end-rslreg_zero_stub
        ldir

        ; Preserve the driver's logical current-bank variable at 7D2EH, but
        ; suppress the physical mapper write.  libkss ignores OUT (FEH),A;
        ; doing it on an MSX would hide the flat song immediately before the
        ; engine copies its MWM header into the channel setup tables.
        xor     a                   ; NOP opcode
        ld      (0x7E88),a
        ld      (0x7E89),a

        xor     a
        call    KSS_INIT
        di

        ; The engine normally chains the old H.KEYI bytes from 46ACH.  This
        ; diagnostic build drives PLAY itself, outside BIOS interrupt context.
        ld      hl,poll_return_stub
        ld      de,0x46AC
        ld      bc,poll_return_stub_end-poll_return_stub
        ldir
        ld      hl,msg_playing
        ld      de,0x0118
        call    vram_print

        ; Poll the real OPL4 timer IRQ.  ALMOSEND programs timer A to 32H,
        ; approximately 60 Hz.  PLAY acknowledges/rearms it at entry.
play_loop:
        in      a,(0xC4)
        rla
        jr      nc,play_loop
        call    KSS_PLAY
        jr      play_loop

rslreg_zero_stub:
        xor     a
        ret
rslreg_zero_stub_end:

poll_return_stub:
        ret
        nop
        nop
        nop
        nop
poll_return_stub_end:

; Write a zero-terminated string directly to the SCREEN 0 name table while DI.
vram_print:
        ld      a,e
        out     (0x99),a
        ld      a,d
        and     0x3F
        or      0x40
        out     (0x99),a
vram_print_loop:
        ld      a,(hl)
        or      a
        ret     z
        out     (0x98),a
        inc     hl
        jr      vram_print_loop

msg_loading_engine:
        defm    "DECOMPRESSING ENGINE",0
msg_engine_loaded:
        defm    "ENGINE READY",0
msg_song_loaded:
        defm    "SONG READY",0
msg_playing:
        defm    "PLAYING ALMOSEND",0

        ; Official forward ZX0 decoder, relocated by the host build to BF00H.
        ; z80asm's ORG does not emit a physical gap in a raw binary, so pad
        ; explicitly to keep the BLOAD address and decoder address identical.
        defs    ZX0_DECODER-$,0
        incbin  '@@DZX0@@'
