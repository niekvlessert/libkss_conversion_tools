; Standalone MSX loader/player for f1spirit_16kb_max_compressed.kss.
;
; F1DATA.BIN is first BLOADed at 4000H.  This player moves the compact KSS
; load image to 0200H, expands the engine and its one 16K bank, and calls the
; original PLAY routine once per VDP interrupt.

        org     0xC000

BIOS_ENASLT:    equ     0x0024
BIOS_CHPUT:     equ     0x00A2
BIOS_RAMAD1:    equ     0xF342

DATA_SOURCE:    equ     0x4000
DATA_TARGET:    equ     0x0200
DATA_LENGTH:    equ     @@DATA_LENGTH@@
INIT_ADDRESS:   equ     0x0200
PLAY_ADDRESS:   equ     0x5F80
RUNTIME_ADDRESS: equ    0x5000
INIT_TRAMPOLINE_ADDRESS: equ 0x500D
ENGINE_SEGMENT: equ     2
BANK_SEGMENT:   equ     14
STACK_TOP:      equ     0xF000

start:
        di
        ld      sp,STACK_TOP
        ld      a,'1'
        call    BIOS_CHPUT

        ; Make pages 1 and 2 ordinary main RAM while BIOS is still visible.
        ld      a,(BIOS_RAMAD1)
        ld      hl,0x4000
        call    BIOS_ENASLT
        di
        ld      a,(BIOS_RAMAD1)
        ld      hl,0x8000
        call    BIOS_ENASLT
        di
        ld      a,ENGINE_SEGMENT
        out     (0xFD),a

        ld      a,'2'
        call    BIOS_CHPUT
        di

        ; INIT calls BIOS entry 0093H. Save the original primary-slot layout
        ; so a page-1 trampoline can restore BIOS after ZX0 leaves page 0.
        in      a,(0xA8)
        ld      (runtime_original_a8+1),a

        ; Put main RAM in page 0.  ENASLT itself executes in page 0 and cannot
        ; safely unmap the BIOS underneath its own return path, so switch the
        ; primary and (when present) secondary slot directly from page 3.
        call    map_ram_page0
        di

        ; The compressed image then lives at its original KSS load address.
        ld      hl,DATA_SOURCE
        ld      de,DATA_TARGET
        ld      bc,DATA_LENGTH
        ldir

        ; Preserve the post-INIT loop below the expanded engine.  Returning
        ; straight here also avoids depending on C000H surviving engine INIT.
        ld      hl,runtime_code
        ld      de,RUNTIME_ADDRESS
        ld      bc,runtime_code_end-runtime_code
        ldir

        ; The compressed bootstrap normally jumps directly to 5F00H. Route
        ; its final jump through page 1, where BIOS can safely be restored.
        ld      hl,INIT_TRAMPOLINE_ADDRESS
        ld      (DATA_TARGET+29),hl

        ; The patched engine selects bank 14 with OUT (FEH),A.  Expand the
        ; compressed bank into that exact physical mapper segment.
        ld      a,ENGINE_SEGMENT
        out     (0xFD),a
        ld      a,BANK_SEGMENT
        out     (0xFE),a

        ld      sp,STACK_TOP
        ld      hl,RUNTIME_ADDRESS
        push    hl
        xor     a                       ; KSS song index 0
        jp      INIT_ADDRESS

map_ram_page0:
        ld      a,(BIOS_RAMAD1)
        ld      b,a
        bit     7,b
        jr      z,map_ram_page0_primary
        ; FFFFH reads as the inverted secondary-slot register.
        ld      a,(0xFFFF)
        cpl
        and     0xFC
        ld      c,a
        ld      a,b
        rrca
        rrca
        and     0x03
        or      c
        ld      (0xFFFF),a
map_ram_page0_primary:
        in      a,(0xA8)
        and     0xFC
        ld      c,a
        ld      a,b
        and     0x03
        or      c
        out     (0xA8),a
        ret

runtime_code:
        ld      sp,STACK_TOP
        im      1
runtime_loop:
        ei
        halt
        di                              ; PLAY temporarily repurposes SP
        call    PLAY_ADDRESS
        jr      runtime_loop
runtime_init_trampoline:
runtime_original_a8:
        ld      a,0x00                  ; immediate patched before page-0 switch
        out     (0xA8),a                ; restore BIOS and original RAM slots
        jp      0x5F00
runtime_code_end:
