; Disk-BASIC launcher for the compressed Nemesis 3 KSSX image.
;
; NEM3COMP.KSS is BLOAD-wrapped by the host builder and is loaded at 4000H.
; This program expands the engine and three 16K banks before playback, and then leaves the engine's
; original OUT (FE),A bank selectors untouched.
;
; The 16K music banks use the MSX RAM mapper at page 2 (8000H-BFFFH). This
; requires a machine with an MSX memory mapper; the OpenMSX Panasonic_FS-A1ST
; configuration provides one.

        org     0xC000
        jp      start

BIOS_ENASLT:    equ     0x0024
BIOS_CHPUT:     equ     0x00A2
BIOS_RAMAD1:    equ     0xF342
BIOS_RAMAD2:    equ     0xF343
H_TIMI:         equ     0xFD9F
KSS_SOURCE:     equ     0x4000
ENGINE_ADDRESS: equ     0x5FC0
PLAY_ADDRESS:   equ     0x5FE0
BANK1_SCRATCH:  equ     0xD100
; Keep the decoder below the staging buffer.  Use the unused RAM gap below
; the decoder for the temporary stack; Disk-BASIC's high work area is not
; safe to reuse while the loader is calling BIOS routines.
ZX0_DECODER:    equ     0xCF00
STACK_TOP:      equ     0xC800
PHASE_MARKER:   equ     0xC600
RUNTIME_ADDRESS: equ    0x4000
RUNTIME_PLAY_ADDRESS: equ 0x4040
RUNTIME_BANK:   equ     12             ; dedicated RAM segment not used by KSS
BANK0:          equ     9
BANK1:          equ 10
BANK2:          equ 11
SOURCE_BANK:    equ     1             ; Page-2 segment left by Disk-BASIC BLOAD.
TRACK_ID:       equ 4             ; KSSX song 0 maps to Nemesis 3 track 4.

ENGINE_OFFSET:  equ 0x007A        ; KSS header + bootstrap + 68-byte decoder.
ENGINE_LENGTH:  equ 0x1407        ; 5127 bytes.
BANK0_OFFSET:   equ 0x14A3        ; N3ZX header + 10 + stream 0.
BANK0_LENGTH:   equ 0x2AF8
BANK1_OFFSET:   equ 0x3F9B
BANK1_LENGTH:   equ 0x122D
BANK1_PREFIX:   equ 0x0065
BANK1_SUFFIX:   equ 0x11C8
BANK2_OFFSET:   equ 0x51C8
BANK2_LENGTH:   equ 0x13BA

start:
        di
        ld      sp,STACK_TOP
        ld      a,'1'
        call    show_phase
        di

        ; Make pages 1 and 2 ordinary RAM.  Page 2 must be explicitly mapped
        ; before reading the KSS tail and using the RAM mapper at port FEH.
        ; Use the same verified main-RAM slot as page 1. Disk-BASIC's
        ; RAMAD2 value can describe the slot currently visible at 8000H,
        ; which is not necessarily the mapper slot holding the BLOAD data.
        ld      a,(BIOS_RAMAD1)
        ld      hl,0x8000
        call    BIOS_ENASLT
        di
        ld      a,(BIOS_RAMAD1)
        ld      hl,0x4000
        call    BIOS_ENASLT
        ; ENASLT is a BIOS routine and may return with interrupts enabled.
        ; Keep the decoder and mapper construction atomic until H.TIMI is
        ; replaced below.
        di
        ; Disk-BASIC may leave the RAM-mapper page-1 segment changed.  The
        ; binary BLOAD image was laid out using the standard 3-2-1-0 mapping.
        ld      a,2
        out     (0xFD),a
        ld      a,(KSS_SOURCE+BANK1_OFFSET)
        cp      0x14
        jr      z,source_probe_ok
        ld      a,'Q'
        call    show_phase
        di
        jr      source_probe_done
source_probe_ok:
        ld      a,'R'
        call    show_phase
        di
source_probe_done:
        ld      hl,KSS_SOURCE+BANK1_OFFSET
        ld      de,bank1_probe_bytes
        ld      b,16
source_probe_compare:
        ld      a,(hl)
        ld      c,a
        ld      a,(de)
        cp      c
        jr      nz,source_probe_compare_bad
        inc     hl
        inc     de
        djnz    source_probe_compare
        ld      a,'S'
        call    show_phase
        di
        jr      source_probe_compare_done
source_probe_compare_bad:
        ld      a,'T'
        call    show_phase
        di
source_probe_compare_done:

        ; Preserve bank 1 before engine expansion. The 2040H-byte engine
        ; destination at 5FC0H reaches 8000H and overlaps the bank-1 prefix
        ; in the source image.
        call    find_source_bank
        ld      a,'A'
        call    show_phase
        di

        ; Decode bank 1 while its compressed stream is in the safe buffer.
        ld      a,BANK1
        out     (0xFE),a
        ld      a,'X'
        call    show_phase
        di
        call    check_bank1_stage
        ld      hl,BANK1_SCRATCH
        ld      de,0xA000
        call    ZX0_DECODER
        ld      sp,STACK_TOP
        ld      a,'3'
        call    show_phase
        di

        ; Decode bank 0 before engine expansion. Its compressed source overlaps
        ; the later engine destination, but its decoded mapper bank does not.
        ld      a,'5'
        call    show_phase
        di
        ; BIOS_CHPUT can leave the RAM mapper page-1 segment changed.
        ld      a,2
        out     (0xFD),a
        ld      a,BANK0
        out     (0xFE),a
        ld      hl,KSS_SOURCE+BANK0_OFFSET
        ld      de,0x8000
        call    ZX0_DECODER
        ld      sp,STACK_TOP
        ld      a,'6'
        call    show_phase
        di
        ld      a,2
        out     (0xFD),a

        ; Decode the engine directly from the verified page-1 source.
        ld      hl,KSS_SOURCE+ENGINE_OFFSET
        ld      de,ENGINE_ADDRESS
        call    ZX0_DECODER
        ld      sp,STACK_TOP
        ld      a,'2'
        call    show_phase
        di
        ld      a,(candidate_bank)
        out     (0xFE),a

        ; Restore the mapper segment containing the BLOAD source and reuse
        ; the safe buffer for the second upper-bank stream.
        ld      a,(candidate_bank)
        out     (0xFE),a
        ld      hl,KSS_SOURCE+BANK2_OFFSET
        ld      de,BANK1_SCRATCH
        ld      bc,BANK2_LENGTH
        ldir
        ld      a,'B'
        call    show_phase
        di

        ld      a,BANK2
        out     (0xFE),a
        ld      hl,BANK1_SCRATCH
        ld      de,0xA000
        call    ZX0_DECODER
        ld      sp,STACK_TOP
        ld      a,'4'
        call    show_phase
        di

        ; Copy the shared lower 8K into banks 10 and 11. The source and
        ; destination windows are the same, so this deliberately switches the
        ; mapper once per byte. This happens only during loading, never during
        ; playback, and keeps the loader independent of extra scratch RAM.
        ld      a,BANK1
        call    copy_common_to_bank
        ld      a,BANK2
        call    copy_common_to_bank
        ld      a,'7'
        call    show_phase
        di
        ; Keep the expanded engine visible in the source page-1 RAM segment.
        ld      a,2
        out     (0xFD),a

        ; The engine's INIT uses the C000H work area, so preserve the
        ; persistent INIT/PLAY trampoline before entering it.  The dedicated
        ; mapper segment is not touched by the engine's normal page-1 work.
        ld      a,RUNTIME_BANK
        out     (0xFD),a
        ld      hl,runtime_code
        ld      de,RUNTIME_ADDRESS
        ld      bc,runtime_code_end-runtime_code
        ldir
        ld      a,2
        out     (0xFD),a

        ; The engine uses both C000H and page 1 as workspace during INIT, so
        ; enter the preserved trampoline after INIT returns.
        ld      a,'8'
        call    show_phase
        di
        ld      a,RUNTIME_BANK
        out     (0xFD),a
        jp      RUNTIME_ADDRESS

; Print one phase digit without changing the caller's registers. BIOS calls
; may alter interrupt state, so each caller disables interrupts again.
show_phase:
        ld      (PHASE_MARKER),a
        push    af
        push    bc
        push    de
        push    hl
        call    BIOS_CHPUT
        ld      a,' '
        call    BIOS_CHPUT
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=target bank. Bank 9 contains the common lower half. Map the source into
; page 1 and the destination into page 2, so the whole 8K can be copied with
; one LDIR instead of switching the same mapper window for every byte.
copy_common_to_bank:
        ld      d,a
        ld      a,BANK0
        out     (0xFD),a
        ld      a,d
        out     (0xFE),a
        ld      hl,0x4000
        ld      de,0x8000
        ld      bc,0x2000
        ldir
        ld      a,2
        out     (0xFD),a
        ret

runtime_h_timi_entry:
        jp      PLAY_ADDRESS
        nop
        nop

runtime_code:
runtime_init:
        ld      sp,STACK_TOP
        xor     a
        ld      bc,0
        ld      de,0
        ld      hl,0
        ld      ix,0
        ld      iy,0
        ; The engine itself is in the normal page-1 segment.  The runtime
        ; code is in the dedicated segment and is restored after INIT.
        ld      a,2
        out     (0xFD),a
        ld      a,TRACK_ID
        call    ENGINE_ADDRESS
        ; INIT may leave page 1 on another mapper segment.  Select the
        ; dedicated runtime segment and install the H.TIMI hook afterwards.
        ld      a,RUNTIME_BANK
        out     (0xFD),a
        ld      a,0xC3
        ld      (H_TIMI),a
        ld      a,0xE0
        ld      (H_TIMI+1),a
        ld      a,0x5F
        ld      (H_TIMI+2),a
        xor     a
        ld      (H_TIMI+3),a
        ld      (H_TIMI+4),a
runtime_wait:
        ; Keep the CPU in a BIOS routine while H.TIMI drives PLAY.  The
        ; wait stub is placed in page-2 segment 2, which remains available
        ; after INIT and does not depend on the page-1 runtime mapping.
        ld      a,2
        out     (0xFE),a
        ld      a,0xC3
        ld      (0x8000),a
        xor     a
        ld      (0x8001),a
        ld      a,0x9F
        ld      (0x8002),a
        ld      sp,STACK_TOP
        im      1
        ei
runtime_wait_loop:
        call    0x8000
        jr      runtime_wait_loop
runtime_code_end:

common_byte:
        defb    0

candidate_bank:
        defb    0

prefix_error:
        defb    0

bank1_probe_bytes:
        defb    0x14,0xE8,0xFF,0xFE,0x01,0xEE,0x01,0xE9
        defb    0x07,0xEA,0x07,0xEB,0x03,0x70,0xD1,0xB0

bank1_prefix_expected:
        defb    0x14,0xE8,0xFF,0xFE,0x01,0xEE,0x01,0xE9
        defb    0x07,0xEA,0x07,0xEB,0x03,0x70,0xD1,0xB0
        defb    0xD0,0x20,0x70,0xB0,0xEB,0x04,0x70,0x77
        defb    0xD4,0x9A,0xF8,0x1D,0x08,0xAA,0x12,0xD3
        defb    0xA6,0xD2,0x0F,0xEA,0x05,0xEB,0x06,0xD0
        defb    0x20,0xE8,0xF2,0x20,0xF1,0x41,0xC8,0x6A
        defb    0x09,0xDD,0x25,0x32,0xD2,0xD1,0x0A,0x08
        defb    0xA5,0x03,0xE6,0x6C,0xFF,0x71,0x9C,0xCD
        defb    0x88,0xD0,0xFF,0xD2,0xB7,0x3A,0xCA,0x23
        defb    0xDD,0x27,0xA7,0x9B,0xB1,0x8A,0x9C,0x76
        defb    0x02,0xEF,0x06,0x1A,0x70,0xF7,0x05,0xF6
        defb    0xFD,0xA1,0x06,0xF7,0xD3,0xDA,0x39,0x42
        defb    0x81,0xE3,0x04,0xE9,0x08

; Return Z when the staged page-1 prefix matches every expected byte.
bank1_prefix_exact:
        ld      hl,BANK1_SCRATCH
        ld      de,bank1_prefix_expected
        ld      bc,BANK1_PREFIX
bank1_exact_loop:
        ld      a,(hl)
        push    hl
        push    de
        push    bc
        ld      c,a
        ld      a,(de)
        cp      c
        pop     bc
        pop     de
        pop     hl
        ret     nz
        inc     hl
        inc     de
        dec     bc
        ld      a,b
        or      c
        jr      nz,bank1_exact_loop
        xor     a
        ret

; Locate the page-2 RAM-mapper segment containing the continuation of the
; bank-1 stream. The first part is in page 1, so each candidate can be tested
; by copying the complete stream to the page-3 staging buffer.
find_source_bank:
        ; First verify the part that is still in page 1.
        ld      hl,KSS_SOURCE+BANK1_OFFSET
        ld      de,BANK1_SCRATCH
        ld      bc,BANK1_PREFIX
        ldir
        call    bank1_prefix_exact
        jr      z,find_source_bank_exact_ok
        ld      a,'W'
        call    show_phase
        di
find_source_bank_exact_halt:
        jr      find_source_bank_exact_halt
find_source_bank_exact_ok:
        ld      a,'V'
        call    show_phase
        di
        call    bank1_prefix_match
        jr      z,find_source_bank_prefix_ok
        ld      a,(prefix_error)
        call    show_phase
        di
find_source_bank_prefix_halt:
        jr      find_source_bank_prefix_halt
find_source_bank_prefix_ok:
        ld      a,'P'
        call    show_phase
        di

        ; The BLOAD image continues in mapper segment 1 at CPU 8000H.
        ; Probe that known first byte before scanning all segments.
        ld      a,1
        out     (0xFE),a
        ld      a,(0x8000)
        cp      0xD0
        jr      z,find_source_bank_page2_probe_ok
        ld      a,'U'
        call    show_phase
        di
        jr      find_source_bank_page2_probe_done
find_source_bank_page2_probe_ok:
        ld      a,'Y'
        call    show_phase
        di
find_source_bank_page2_probe_done:
        xor     a
        ld      (candidate_bank),a
find_source_bank_loop:
        ld      a,(candidate_bank)
        out     (0xFE),a
        ld      hl,0x8000
        ld      de,BANK1_SCRATCH+BANK1_PREFIX
        ld      bc,BANK1_SUFFIX
        ldir
        call    bank1_suffix_match
        jr      z,find_source_bank_found
        ld      a,(candidate_bank)
        inc     a
        ld      (candidate_bank),a
        cp      0x40
        jr      c,find_source_bank_loop
        ld      a,'E'
        call    show_phase
        di
find_source_bank_halt:
        jr      find_source_bank_halt
find_source_bank_found:
        ld      a,'M'
        call    show_phase
        ld      a,(candidate_bank)
        add     a,'0'
        call    show_phase
        di
        ret

; Return Z when the staged page-2 suffix has the expected 16-bit byte sum.
bank1_suffix_match:
        ld      hl,BANK1_SCRATCH+BANK1_PREFIX
        ld      bc,BANK1_SUFFIX
        ld      de,0
bank1_suffix_loop:
        ld      a,(hl)
        add     a,e
        ld      e,a
        jr      nc,bank1_suffix_no_carry
        inc     d
bank1_suffix_no_carry:
        inc     hl
        dec     bc
        ld      a,b
        or      c
        jr      nz,bank1_suffix_loop
        ld      a,d
        cp      0x73
        ret     nz
        ld      a,e
        cp      0xAF
        ret

; Return Z when the staged page-1 prefix has the expected 16-bit byte sum.
bank1_prefix_match:
        ld      hl,BANK1_SCRATCH
        ld      bc,BANK1_PREFIX
        ld      de,0
bank1_prefix_loop:
        ld      a,(hl)
        add     a,e
        ld      e,a
        jr      nc,bank1_prefix_no_carry
        inc     d
bank1_prefix_no_carry:
        inc     hl
        dec     bc
        ld      a,b
        or      c
        jr      nz,bank1_prefix_loop
        ld      a,d
        cp      0x35
        jr      nz,bank1_prefix_high_bad
        ld      a,e
        cp      0x4C
        jr      z,bank1_prefix_match_ok
        ld      a,'L'
        ld      (prefix_error),a
        ld      a,1
        or      a
        ret
bank1_prefix_high_bad:
        ld      a,'H'
        ld      (prefix_error),a
        ld      a,1
        or      a
        ret
bank1_prefix_match_ok:
        ret

; Check the staged compressed stream before giving it to ZX0.  The expected
; 16-bit byte sum for the bank-1 stream is A8FBH.
check_bank1_stage:
        ld      hl,BANK1_SCRATCH
        ld      bc,BANK1_LENGTH
        ld      de,0
check_bank1_loop:
        ld      a,(hl)
        add     a,e
        ld      e,a
        jr      nc,check_bank1_no_carry
        inc     d
check_bank1_no_carry:
        inc     hl
        dec     bc
        ld      a,b
        or      c
        jr      nz,check_bank1_loop
        ld      a,d
        cp      0xA8
        jr      nz,check_bank1_bad
        ld      a,e
        cp      0xFB
        jr      nz,check_bank1_bad
        ld      a,'C'
        jr      check_bank1_report
check_bank1_bad:
        ld      a,'E'
check_bank1_report:
        call    show_phase
        di
        ret

        ; Relocated forward ZX0 decoder. The host builder inserts it at D000H.
        defs    ZX0_DECODER-$,0
        incbin  '@@DZX0@@'
