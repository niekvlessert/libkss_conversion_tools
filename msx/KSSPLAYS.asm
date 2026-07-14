; Five-track selector for DMV_5_music.rom.
;
; The menu uses BIOS calls before the MBWave engine is initialized.  Once a
; track is selected, playback follows the same flat-memory contract as the
; working KSPPLAYR.BIN player.

        org     0xC000
        jp      start

BIOS_RDSLT:       equ     0x000C
BIOS_ENASLT:      equ     0x0024
BIOS_CHGET:       equ     0x009F
BIOS_CHPUT:       equ     0x00A2
BIOS_RAMAD1:      equ     0xF342
BIOS_RAMAD2:      equ     0xF343
KSS_INIT:         equ     0x7F30
KSS_PLAY:         equ     0x45E5
ZX0_DECODER:      equ     0xCF00
STACK_TOP:        equ     0xF300
ENGINE_SCRATCH:   equ     0x8000
SONG_SCRATCH:     equ     0xD000

ENGINE_BANK0:     equ     2
ENGINE_FIRST_SRC: equ     0x4020
ENGINE_FIRST_LEN: equ     0x1FE0
ENGINE_SECOND_LEN:equ     0x04AC

start:
        im      1
        ld      sp,STACK_TOP
        ei
        ld      hl,msg_menu
        call    print_string

select_track:
        call    BIOS_CHGET
        cp      '1'
        jr      c,select_track
        cp      '6'
        jr      nc,select_track
        push    af
        call    BIOS_CHPUT
        pop     af
        sub     '1'
        ld      (selected_track),a
        ld      a,13
        call    BIOS_CHPUT
        ld      a,10
        call    BIOS_CHPUT
        ld      hl,msg_loading
        call    print_string
        di

        ; The mapper cartridge occupies page 2 after boot.  Select main RAM
        ; before using 8000H as the materialized song window.
        ld      a,(BIOS_RAMAD2)
        ld      h,0x80
        call    BIOS_ENASLT

        call    find_data_cartridge
        jp      c,hang
        ld      (cart_slot),a

        ; Copy and decode the shared engine.
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

        ; Load the selected SONG table entry into working variables.
        ld      a,(selected_track)
        ld      hl,song_table
        or      a
        jr      z,song_entry_ready
        ld      b,a
        ld      de,7
song_entry_step:
        add     hl,de
        djnz    song_entry_step
song_entry_ready:
        ld      a,(hl)
        ld      (rom_bank),a
        inc     hl
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        ld      (song_source),de
        inc     hl
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        ld      (song_packed_size),bc
        inc     hl
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        ld      (song_raw_size),de

        call    map_cartridge_page1
        ld      a,(rom_bank)
        ld      (0x5000),a
        ld      hl,(song_source)
        ld      bc,(song_packed_size)
        ld      de,SONG_SCRATCH
        call    copy_banked_rom
        call    map_ram_page1
        ld      hl,SONG_SCRATCH
        ld      de,0x8000
        call    ZX0_DECODER
        call    compact_mwm

        ; Install the compact-KSP MBWave bootstrap and libkss compatibility
        ; shims used by the proven single-track player.
        ld      hl,mbwave_bootstrap
        ld      de,KSS_INIT
        ld      bc,mbwave_bootstrap_end-mbwave_bootstrap
        ldir
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

hang:
        jr      hang

; Copy BC bytes from the current 8 KiB cartridge bank/window to DE.  HL may
; begin anywhere in 4000H-5FFFH; crossing 6000H advances the physical bank.
copy_banked_rom:
        ld      a,b
        or      c
        ret     z
copy_banked_rom_loop:
        ld      a,(hl)
        ld      (de),a
        inc     hl
        inc     de
        dec     bc
        ld      a,b
        or      c
        ret     z
        ld      a,h
        cp      0x60
        jr      nz,copy_banked_rom_loop
        ld      a,(rom_bank)
        inc     a
        ld      (rom_bank),a
        ld      (0x5000),a
        ld      hl,0x4000
        jr      copy_banked_rom_loop

; Convert a decompressed runtime MWM at 8000H to MBWave's flat pattern form.
; This is the Z80 equivalent of ksp_compact_mwm().
compact_mwm:
        ld      a,(0x8006)         ; song length
        inc     a                  ; position count
        ld      (position_count),a
        ld      b,a
        ld      hl,0x811C          ; signature + 116H-byte header
        xor     a
        ld      d,a                ; maximum pattern number
compact_find_max:
        ld      a,(hl)
        cp      d
        jr      c,compact_not_max
        ld      d,a
compact_not_max:
        inc     hl
        djnz    compact_find_max
        ld      a,d
        inc     a
        ld      (patterns_left),a
        ld      c,a
        ld      b,0
        add     hl,bc
        add     hl,bc              ; skip two-byte pattern offsets
        push    hl
        pop     de                 ; compact destination starts here

compact_block:
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        inc     hl
        ld      a,(hl)
        inc     hl
        push    af
        ldir
        pop     af
        ld      c,a
        ld      a,(patterns_left)
        sub     c
        ld      (patterns_left),a
        jr      nz,compact_block

        ; Preserve any bytes following the final pattern group.
        push    hl
        ld      bc,(song_raw_size)
        ld      a,c
        add     a,0x00             ; raw end is 8000H + unpacked size
        ld      c,a
        ld      a,b
        adc     a,0x80
        ld      b,a
        pop     hl
        ld      a,c
        sub     l
        ld      c,a
        ld      a,b
        sbc     a,h
        ld      b,a
        ldir
        ret

; Return the primary slot containing the distinctive data-ROM header in A.
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
        or      a
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

print_string:
        ld      a,(hl)
        or      a
        ret     z
        call    BIOS_CHPUT
        inc     hl
        jr      print_string

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

; bank, CPU source, packed size, unpacked size
song_table:
        defb    3
        defw    0x44D0,0x1666,0x2FB9       ; 1 ALMOSEND
        defb    3
        defw    0x5B36,0x179A,0x3685       ; 2 ANGEL_01
        defb    12
        defw    0x5481,0x144B,0x2F71       ; 3 ANGEL_06
        defb    13
        defw    0x48CC,0x16BE,0x33D5       ; 4 ANGEL_09
        defb    13
        defw    0x5F8A,0x110C,0x25C4       ; 5 CLIMAX

msg_menu:
        defm    13,10,"DMV ROM MOONSOUND PLAYER",13,10
        defm    "1 ALMOSEND",13,10
        defm    "2 ANGEL_01",13,10
        defm    "3 ANGEL_06",13,10
        defm    "4 ANGEL_09",13,10
        defm    "5 CLIMAX",13,10
        defm    "SELECT 1-5: ",0
msg_loading:
        defm    "LOADING FROM ROM...",13,10,0

cart_slot:
        defb    0
slot_candidate:
        defb    0
selected_track:
        defb    0
rom_bank:
        defb    0
position_count:
        defb    0
patterns_left:
        defb    0
song_raw_size:
        defw    0
song_source:
        defw    0
song_packed_size:
        defw    0

        defs    ZX0_DECODER-$,0
        incbin  '@@DZX0@@'
