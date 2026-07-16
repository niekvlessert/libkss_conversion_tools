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
BIOS_SNSMAT:      equ     0x0141
BIOS_RAMAD1:      equ     0xF342
BIOS_RAMAD2:      equ     0xF343
KSS_INIT:         equ     0x7F30
KSS_PLAY:         equ     0x45E5
ZX0_DECODER:      equ     0xCF00
STACK_TOP:        equ     0xF300
BASIC_TRACK:      equ     0xBFFF
ENGINE_SCRATCH:   equ     0x8000
SONG_SCRATCH:     equ     0xD000

ENGINE_BANK0:     equ     2
ENGINE_FIRST_SRC: equ     0x4020
ENGINE_FIRST_LEN: equ     0x1FE0
ENGINE_SECOND_LEN:equ     0x04AC

start:
        im      1
        ld      sp,STACK_TOP
        ld      a,(BASIC_TRACK)
        cp      5
        jr      c,selected_track_valid
        xor     a
selected_track_valid:
        ld      (selected_track),a
        ei
        ld      hl,msg_loading
        call    print_string

load_selected_track:
        di

        ; The mapper cartridge occupies page 2 after boot.  Select main RAM
        ; before using 8000H as the materialized song window.
        ld      a,(BIOS_RAMAD2)
        ld      h,0x80
        call    BIOS_ENASLT

        call    find_data_cartridge
        jp      c,hang
        ld      (switch_cart_slot),a
cartridge_ready:

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
load_selected_song:
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
        call    compact_selected_mwm

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
compact_selected_mwm:
        ld      a,(selected_track)
        ld      hl,compact_table
        or      a
        jr      z,compact_entry_ready
        ld      b,a
        ld      de,6
compact_entry_step:
        add     hl,de
        djnz    compact_entry_step
compact_entry_ready:
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        inc     hl
        push    bc
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        inc     hl
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        pop     hl
        ldir
        ret

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
        ld      a,(switch_cart_slot)
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

; source, destination, length after the MWM has been decoded at 8000H.
; Every song in this five-track ROM has one pattern block, so one overlapping
; forward LDIR removes its three-byte block header and preserves its tail.
compact_table:
        defw    0x81B8,0x81B5,0x2E01       ; 1 ALMOSEND
        defw    0x81F7,0x81F4,0x348E       ; 2 ANGEL_01
        defw    0x81F4,0x81F1,0x2D7D       ; 3 ANGEL_06
        defw    0x823C,0x8239,0x3199       ; 4 ANGEL_09
        defw    0x81F7,0x81F4,0x23CD       ; 5 CLIMAX

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
switch_cart_slot:
        defb    0

        defs    ZX0_DECODER-$,0
        incbin  '@@DZX0@@'

; Runtime switching extension.  Keep this after the proven CF00H decoder so
; adding controls does not relocate any engine/bootstrap material below it.
play_and_controls:
        call    KSS_PLAY
        call    read_escape
        jp      z,control_escape
        call    read_track_key
        ret     nc
        push    af
        call    stop_moonsound
        call    restore_hkeyi
        call    wait_track_release
        pop     af
        ld      (selected_track),a
        xor     a
        ld      (0x4535),a         ; NOP MBPLAY's EI on restart
        jp      load_selected_song

control_escape:
        call    stop_moonsound
        call    restore_hkeyi
        call    wait_escape_release
control_wait_track:
        call    read_track_key
        jr      nc,control_wait_track
        push    af
        call    wait_track_release
        pop     af
        ld      (selected_track),a
        xor     a
        ld      (0x4535),a
        jp      load_selected_song

read_escape:
        ld      a,7
        call    BIOS_SNSMAT
        bit     2,a
        ret

wait_escape_release:
wait_escape_release_loop:
        ld      a,7
        call    BIOS_SNSMAT
        bit     2,a
        jr      z,wait_escape_release_loop
        ret

read_track_key:
        xor     a
        call    BIOS_SNSMAT
        cpl
        and     0x3E
        ld      e,a
        ld      a,e
        or      a
        ret     z
        srl     a
        ld      b,5
        ld      c,0
read_track_key_loop:
        rrca
        jr      c,read_track_key_found
        inc     c
        djnz    read_track_key_loop
        or      a
        ret
read_track_key_found:
        ld      a,c
        scf
        ret

wait_track_release:
wait_track_release_loop:
        xor     a
        call    BIOS_SNSMAT
        and     0x3E
        cp      0x3E
        jr      nz,wait_track_release_loop
        ret

restore_hkeyi:
        ld      a,0xC9
        ld      hl,0xFD9A
        ld      b,5
restore_hkeyi_loop:
        ld      (hl),a
        inc     hl
        djnz    restore_hkeyi_loop
        ret

stop_moonsound:
        ld      a,4
        out     (0xC0),a
        ld      a,0x80
        out     (0xC1),a
        ld      a,4
        out     (0xC0),a
        ld      a,0x60
        out     (0xC1),a
        ld      d,0xB0
        ld      b,9
stop_fm0:
        ld      a,d
        out     (0xC0),a
        xor     a
        out     (0xC1),a
        inc     d
        djnz    stop_fm0
        ld      d,0xB0
        ld      b,9
stop_fm1:
        ld      a,d
        out     (0xC2),a
        xor     a
        out     (0xC3),a
        inc     d
        djnz    stop_fm1
        ld      d,0x68
        ld      b,24
stop_wave:
        ld      a,d
        out     (0xC4),a
        xor     a
        out     (0xC5),a
        inc     d
        djnz    stop_wave
        ret
