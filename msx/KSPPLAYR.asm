; BASIC-launched MoonSound player for track 0 in DMV_5_music.rom.
;
; The data cartridge uses 8 KiB Konami banking and contains a compact KSP at
; physical ROM offset 4000H.  This first version uses its known directory
; entries for ENGN[0] and SONG[0] (ALMOSEND, no MWK).

        org     0xC000
        jp      start

BIOS_RDSLT:       equ     0x000C
BIOS_ENASLT:      equ     0x0024
BIOS_RAMAD1:      equ     0xF342
BIOS_RAMAD2:      equ     0xF343
KSS_INIT:         equ     0x7F30
KSS_PLAY:         equ     0x45E5
ZX0_DECODER:      equ     0xCF00
STACK_TOP:        equ     0xF300
ENGINE_SCRATCH:   equ     0x8000
SONG_SCRATCH:     equ     0xD000

; Absolute ROM layout for DMV_5_music.rom.
ENGINE_BANK0:     equ     2
ENGINE_FIRST_SRC: equ     0x4020
ENGINE_FIRST_LEN: equ     0x1FE0
ENGINE_SECOND_LEN:equ     0x04AC
SONG_BANK:        equ     3
SONG_SRC:         equ     0x44D0
SONG_PACKED_LEN:  equ     0x1666

; ALMOSEND raw MWM has one three-byte block header at file offset 01B5H.
SONG_BLOCK_SRC:   equ     0x81B8
SONG_BLOCK_DST:   equ     0x81B5
SONG_BLOCK_TAIL:  equ     0x2E01

start:
        im      1
        di
        ld      sp,STACK_TOP

        ; A mapper cartridge occupies page 2 after boot.  Put the machine's
        ; RAM there before using it for packed data and the final song.
        ld      a,(BIOS_RAMAD2)
        ld      h,0x80
        call    BIOS_ENASLT
        call    find_data_cartridge
        jp      c,no_cartridge
        ld      (cart_slot),a

        ; Copy the compressed engine from ROM banks 2 and 3 into page 3.
        call    map_cartridge_page1
        ld      a,ENGINE_BANK0
        ld      (0x5000),a
        ld      hl,ENGINE_FIRST_SRC
        ld      de,ENGINE_SCRATCH
        ld      bc,ENGINE_FIRST_LEN
        ldir
        ld      a,ENGINE_BANK0+1
        ld      (0x5000),a
        ld      hl,0x4000
        ld      bc,ENGINE_SECOND_LEN
        ldir

        call    map_ram_page1
        ld      hl,ENGINE_SCRATCH
        ld      de,0x4000
        call    ZX0_DECODER

        ; SONG[0] is entirely in physical ROM bank 3.
        call    map_cartridge_page1
        ld      a,SONG_BANK
        ld      (0x5000),a
        ld      hl,SONG_SRC
        ld      de,SONG_SCRATCH
        ld      bc,SONG_PACKED_LEN
        ldir
        call    map_ram_page1
        ld      hl,SONG_SCRATCH
        ld      de,0x8000
        call    ZX0_DECODER

        ; Reproduce MWMLOAD's compaction for ALMOSEND in place.
        ld      hl,SONG_BLOCK_SRC
        ld      de,SONG_BLOCK_DST
        ld      bc,SONG_BLOCK_TAIL
        ldir

        ; Compact KSP materialization installs this bootstrap at the KSS INIT
        ; address.  It points MBWave at the already materialized song window
        ; and starts playback through the driver's public MBPLAY entry.
        ld      hl,mbwave_bootstrap
        ld      de,KSS_INIT
        ld      bc,mbwave_bootstrap_end-mbwave_bootstrap
        ldir

        ; Match the lightweight libkss execution contract used by KSP.
        ld      a,0xC9
        ld      (0x7CA3),a
        ld      hl,rslreg_zero_stub
        ld      de,0x7CA8
        ld      bc,rslreg_zero_stub_end-rslreg_zero_stub
        ldir
        xor     a
        ld      (0x7E88),a
        ld      (0x7E89),a

        xor     a
        call    KSS_INIT
        di
        ld      hl,poll_return_stub
        ld      de,0x46AC
        ld      bc,poll_return_stub_end-poll_return_stub
        ldir

play_loop:
        in      a,(0xC4)
        rla
        jr      nc,play_loop
        call    KSS_PLAY
        jr      play_loop

no_cartridge:
hang:
        jr      hang

; Return the primary slot containing the distinctive data-ROM header in A.
; Carry is set when no slot matches.
find_data_cartridge:
        xor     a
        ld      (slot_candidate),a
find_slot_loop:
        ld      a,(slot_candidate)
        ld      hl,0x4000
        call    BIOS_RDSLT
        cp      0x41
        jr      nz,next_slot
        ld      a,(slot_candidate)
        ld      hl,0x4001
        call    BIOS_RDSLT
        cp      0x42
        jr      nz,next_slot
        ld      a,(slot_candidate)
        ld      hl,0x4002
        call    BIOS_RDSLT
        cp      0x10
        jr      nz,next_slot
        ld      a,(slot_candidate)
        ld      hl,0x4010
        call    BIOS_RDSLT
        cp      0xC9
        jr      nz,next_slot
        ld      a,(slot_candidate)
        or      a               ; clear carry
        ret
next_slot:
        ld      a,(slot_candidate)
        inc     a
        ld      (slot_candidate),a
        cp      4
        jr      c,find_slot_loop
        scf
        ret

map_cartridge_page1:
        ld      a,(cart_slot)
        ld      h,0x40
        jp      BIOS_ENASLT

map_ram_page1:
        ld      a,(BIOS_RAMAD1)
        ld      h,0x40
        jp      BIOS_ENASLT

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

mbwave_bootstrap:
        xor     a
        ld      hl,0xDA00
        ld      b,0x24
mbwave_bootstrap_clear:
        ld      (hl),a
        inc     hl
        djnz    mbwave_bootstrap_clear
        ld      hl,0x8006
        ld      (0xDA04),hl
        ld      a,0x03
        ld      (0xDA01),a
        ld      (0xDA02),a
        ld      (0xDA03),a
        call    0x4042
        ret
mbwave_bootstrap_end:

cart_slot:
        defb    0
slot_candidate:
        defb    0

        defs    ZX0_DECODER-$,0
        incbin  '@@DZX0@@'
