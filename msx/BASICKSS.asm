; BASIC-launched raw KSS/KSSX player.
;
; BASICKSS.BAS loads this image without ,R, reads an unmodified KSS file in
; 255-byte random records, and calls COPY_FIELD after every GET.  The loader
; keeps the raw file in primary mapper banks 12 and up, relocates itself to
; mapper bank 4, materializes the KSS initial image, and then jumps through
; the KSS INIT/PLAY contract.
;
; The layout intentionally follows the normal Disk-BASIC mapper state:
;   page 1 (4000H) = bank 10
;   page 2 (8000H) = bank 9
;   page 3 (C000H) = bank 8
; KSS 16K bank data remains at the bank numbers declared by the file.  The
; player is copied to bank 7 before it starts materializing the file.

        org     0xC000

BIOS_ENASLT:    equ     0x0024
BIOS_CHPUT:     equ     0x00A2
BIOS_RAMAD1:    equ     0xF342
H_TIMI:         equ     0xFD9F

MAPPER_PAGE1:   equ     0xFD
MAPPER_PAGE2:   equ     0xFE
MAPPER_PAGE3:   equ     0xFF
SCC_SLOT:       equ     0x82    ; openMSX -extb scc, slot B, secondary 0

; These values are filled by the MSX-DOS2 loader before the player starts.
RUNTIME_CONFIG:       equ 0xDFE0
RUNTIME_PLAYER_BANK:  equ RUNTIME_CONFIG
RUNTIME_STAGE_TABLE:  equ RUNTIME_CONFIG+1
RUNTIME_MAIN_TABLE:   equ RUNTIME_STAGE_TABLE+32
RUNTIME_BASIC_P1:     equ RUNTIME_MAIN_TABLE+4
RUNTIME_BASIC_P2:     equ RUNTIME_BASIC_P1+1
RUNTIME_KSS_TABLE:    equ RUNTIME_BASIC_P2+1
RUNTIME_PAGE2_RESTORE: equ RUNTIME_KSS_TABLE+32
RUNTIME_CONFIG_END:   equ RUNTIME_PAGE2_RESTORE+1
STAGE_BANK:           equ 12

; These bytes are outside the BASIC program and are retained while the
; helper temporarily changes the page-2 mapper segment.
BASIC_PTR:      equ     0x7FF0
BASIC_SIZE:     equ     0x7FF3       ; 24-bit little-endian file size
BASIC_STATE:    equ     0x7FF6
BASIC_SONG:     equ     0x7FF7
FIELD_SCRATCH:  equ     0xC800

STACK_TOP:      equ     0xF380
RETURN_STUB:    equ     0xF380
INIT_SONG_SAVED: equ    0xF374
INIT_TARGET:    equ     0xF376
PLAY_WRAPPER:   equ     0xF3A0
PLAY_TARGET:    equ     0xF37A
KSS_TABLE_SAVED: equ    0xF300
SCC_SHADOW:     equ     0xF200
SCC_PLUS_SHADOW: equ    0xF100
KSS_STORAGE_SAVED: equ  0xF37C
RAM_SLOT_SAVED: equ     0xF37D
PAGE2_RESTORE_SAVED: equ 0xF37E
PAGE1_RESTORE_SAVED: equ 0xF37F
BANK0_HANDLER:  equ     0xF500
BANK1_HANDLER:  equ     0xF5C0
CUSTOM_ENASLT:  equ     0xF680

start:
        ld      a,(BASIC_STATE)
        jp      nz,start_player
        jp      copy_field

; Called by BASICKSS.BAS after each GET #1.  The argument written by BASIC
; is VARPTR(B$), i.e. a three-byte string descriptor.  Its word at +1/+2 is
; the address of the FIELD buffer in page 3.
copy_field:
        di
        ld      hl,(BASIC_PTR)
        inc     hl
        ld      e,(hl)
        inc     hl
        ld      d,(hl)

        ; Keep the source in page 3 while page 2 is switched to the staging
        ; mapper segment.  This also handles Disk-BASIC variants whose FIELD
        ; buffer happens to be allocated in page 2.
        ld      hl,FIELD_SCRATCH
        ex      de,hl
        ld      bc,255
        ldir
        ld      hl,FIELD_SCRATCH
        ld      (field_source),hl
        ld      hl,255
        ld      (field_remaining),hl
copy_field_part:
        ld      bc,(field_remaining)
        ld      a,(stage_bank)
        out     (MAPPER_PAGE2),a
        ld      hl,(stage_offset)
        ld      a,h
        and     0x3F
        ld      h,a
        ld      de,0x4000
        ex      de,hl
        or      a
        sbc     hl,de
        call    cap_bc_hl
        ld      (chunk_size),bc

        ld      hl,(stage_offset)
        ld      a,h
        or      0x80
        ld      h,a
        ld      de,(field_source)
        ld      bc,(chunk_size)
        ldir

        ld      hl,(field_source)
        ld      de,(chunk_size)
        add     hl,de
        ld      (field_source),hl
        ld      hl,(field_remaining)
        ld      de,(chunk_size)
        or      a
        sbc     hl,de
        ld      (field_remaining),hl

        ; Keep Disk BASIC's page-2 segment visible when USR returns.
        ld      hl,(stage_offset)
        ld      de,(chunk_size)
        add     hl,de
        ld      a,h
        cp      0x40
        jr      c,copy_field_no_bank_wrap
        sub     0x40
        ld      h,a
        ld      a,(stage_bank)
        inc     a
        ld      (stage_bank),a
        ld      (stage_offset),hl
        ld      a,(field_remaining)
        or      a
        jr      nz,copy_field_part
        jr      copy_field_part_done
copy_field_no_bank_wrap:
        ld      (stage_offset),hl
        ld      a,(field_remaining)
        or      a
        jr      nz,copy_field_part
copy_field_part_done:
        ld      (stage_offset),hl
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        ei
        xor     a
        ret

; Entry used for USR(0) after the file has been staged.
start_player:
        di
        call    map_ram_pages

        ; Move the player off the BASIC page-3 bank.  The KSS image is then
        ; free to overwrite every normal RAM address, including C000H.
        ld      a,(RUNTIME_PLAYER_BANK)
        out     (MAPPER_PAGE2),a
        ld      hl,0xC000
        ld      de,0x8000
        ld      bc,player_end-start
        ldir
        ld      a,(RUNTIME_PLAYER_BANK)
        out     (MAPPER_PAGE3),a
        jp      player_relocated

player_relocated:
        ; Capture BASIC-owned values before page 1 is reused as the source
        ; window for the staged file.
        ld      hl,(BASIC_SIZE)
        ld      (file_size),hl
        ld      a,(BASIC_SIZE+2)
        ld      (file_size+2),a
        ld      a,(BASIC_SONG)
        ld      (song_number),a

        call    read_header
        jr      c,format_error
        call    validate_bank_range
        jr      c,unsupported_bank
        call    check_file_size
        jr      c,file_error

        call    clear_main_ram
        call    copy_initial_image
        jr      c,file_error

        ld      a,(bank_count)
        or      a
        jr      z,no_banked_data
        ld      a,(bank_mode)
        and     0x80
        jr      nz,copy_8k_banks_path
        call    copy_16k_banks
        jr      c,file_error
        jr      no_banked_data
copy_8k_banks_path:
        call    copy_8k_banks
        jr      c,file_error
no_banked_data:

        call    install_page0_stubs
        call    install_runtime_stubs

        ; Install the runtime page-3 trampoline and dynamic state while page
        ; 3 still contains this player.  The final mapper write below is the
        ; last instruction fetched from the relocated player bank.
        call    install_init_trampoline

        ; Page 1 is the KSS RAM view.  Page 2 is either the 8K working RAM
        ; view or the first 16K bank; page 3 becomes the RAM page containing
        ; the return/wrapper/handler code.
        ld      a,(RUNTIME_MAIN_TABLE+3)
        ld      (page3_segment),a
        ld      a,(RUNTIME_MAIN_TABLE+1)
        out     (MAPPER_PAGE1),a

        ld      a,(bank_mode)
        and     0x80
        jr      nz,init_page2_is_ram
        ld      a,(bank_count)
        or      a
        jr      z,init_page2_is_ram
        ld      a,(RUNTIME_KSS_TABLE)
        out     (MAPPER_PAGE2),a
        jr      init_page2_ready
init_page2_is_ram:
        ld      a,(RUNTIME_MAIN_TABLE+2)
        out     (MAPPER_PAGE2),a
init_page2_ready:
        ld      a,(page3_segment)
        out     (MAPPER_PAGE3),a
init_switch_next:
        ; The next bytes are replaced in RAM by init_trampoline before the
        ; page-3 mapper switch.  This label is intentionally not executed
        ; from the relocated player bank.
        nop
        nop
        nop

format_error:
        ld      hl,msg_format
        jr      report_error
file_error:
        ld      hl,msg_file
        jr      report_error
unsupported_bank:
        ld      hl,msg_bank
report_error:
        ; Page 0 is still the BIOS at every error path.
        call    print_string
error_halt:
        jr      error_halt

; Read the first 32 bytes from staged mapper bank 16 into the relocated
; player's private header buffer.
read_header:
        call    map_source_page1
        ld      hl,0x4000
        ld      de,header
        ld      bc,0x20
        ldir
        ld      a,(RUNTIME_BASIC_P1)
        out     (MAPPER_PAGE1),a

        ld      a,(header)
        cp      'K'
        jr      nz,read_header_bad
        ld      a,(header+1)
        cp      'S'
        jr      nz,read_header_bad
        ld      a,(header+2)
        cp      'C'
        jr      z,read_header_kscc
        cp      'S'
        jr      nz,read_header_bad
        ld      a,(header+3)
        cp      'C'
        jr      z,read_header_kscc
        cp      'X'
        jr      nz,read_header_bad
        ld      hl,0x20
        jr      read_header_base_ready
read_header_kscc:
        ld      hl,0x10
read_header_base_ready:
        ld      (source_position),hl
        ld      hl,(header+4)
        ld      (load_address),hl
        ld      hl,(header+6)
        ld      (remaining),hl
        ld      hl,(header+8)
        ld      (init_address),hl
        ld      hl,(header+0x0A)
        ld      (play_address),hl
        ld      a,(header+0x0C)
        ld      (bank_offset),a
        ld      a,(header+0x0D)
        ld      (bank_mode),a
        and     0x7F
        ld      (bank_count),a
        xor     a
        ld      (fast_8k),a
        ld      hl,(load_address)
        ld      a,h
        cp      0x5F
        jr      nz,read_header_fast_done
        ld      a,l
        or      a
        jr      nz,read_header_fast_done
        ld      hl,(play_address)
        ld      a,h
        cp      0x5F
        jr      nz,read_header_fast_done
        ld      a,l
        cp      0x80
        jr      nz,read_header_fast_done
        ld      a,(bank_offset)
        cp      0x0E
        jr      nz,read_header_fast_done
        ld      a,(bank_mode)
        cp      0x82
        jr      nz,read_header_fast_done
        ld      a,(bank_count)
        cp      2
        jr      nz,read_header_fast_done
        ld      a,1
        ld      (fast_8k),a
read_header_fast_done:
        ld      a,(header+0x0E)
        ld      (extra_size),a
        ld      hl,(source_position)
        ld      a,(extra_size)
        ld      e,a
        ld      d,0
        add     hl,de
        ld      (source_position),hl
        xor     a
        ld      (source_position+2),a
        ld      hl,(load_address)
        ld      (destination),hl
        or      a
        ret
read_header_bad:
        scf
        ret

; Bank data is assigned dynamically by the DOS2 loader.
validate_bank_range:
        or      a
        ret

; Check that the initial image and all declared extra data fit inside the
; original file.  This uses 24-bit arithmetic because the staging area is
; deliberately larger than the Z80 address space.
check_file_size:
        ld      hl,(source_position)
        ld      de,(remaining)
        add     hl,de
        ld      a,(source_position+2)
        adc     a,0
        ld      (check_position+2),a
        ld      (check_position),hl
        ld      a,(bank_count)
        ld      b,a
        ld      a,(bank_mode)
        and     0x80
        jr      z,check_file_size_16k_width
        ld      de,0x2000
        jr      check_file_size_bank_width_ready
check_file_size_16k_width:
        ld      de,0x4000
check_file_size_bank_width_ready:
        ld      hl,(check_position)
        ld      a,(check_position+2)
check_file_size_extra_loop:
        ld      c,a
        ld      a,b
        or      a
        jr      nz,check_file_size_extra_add
        ld      a,c
        jr      check_file_size_compare
check_file_size_extra_add:
        ld      a,c
        add     hl,de
        adc     a,0
        djnz    check_file_size_extra_loop
check_file_size_compare:
        ld      c,a
        ld      a,(file_size+2)
        cp      c
        jr      c,check_file_size_bad
        jr      nz,check_file_size_ok
        ld      de,(check_position)
        ex      de,hl
        ld      hl,(file_size)
        or      a
        sbc     hl,de
        jr      c,check_file_size_bad
check_file_size_ok:
        or      a
        ret
check_file_size_bad:
        scf
        ret

; Copy the declared initial image to its KSS load address.  Source is exposed
; through page 1 and the destination through page 2, so the player bank at
; page 3 remains executable even when the image covers C000H.
clear_main_ram:
        xor     a
        ld      (bank_index),a
clear_main_ram_loop:
        ld      e,a
        ld      d,0
        ld      hl,RUNTIME_MAIN_TABLE
        add     hl,de
        ld      a,(hl)
        out     (MAPPER_PAGE2),a
        ld      a,(bank_index)
        or      a
        jr      nz,clear_main_ram_zero
        ld      a,0xC9
        jr      clear_main_ram_fill
clear_main_ram_zero:
        xor     a
clear_main_ram_fill:
        ld      (0x8000),a
        ld      hl,0x8000
        ld      de,0x8001
        ld      bc,0x3FFF
        ldir
        ld      a,(bank_index)
        inc     a
        ld      (bank_index),a
        cp      4
        jr      nz,clear_main_ram_loop
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        ret

copy_initial_image:
        ld      hl,(source_position)
        ld      (source_position),hl
copy_initial_loop:
        ld      bc,(remaining)
        ld      a,b
        or      c
        ret     z
        ld      a,b
        cp      0x40
        jr      c,copy_initial_cap_source
        ld      bc,0x4000
copy_initial_cap_source:
        call    source_boundary
        call    cap_bc_hl
        call    destination_boundary
        call    cap_bc_hl
        ld      (chunk_size),bc

        call    map_source_page1
        call    map_destination_page2
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,(destination)
        ld      a,d
        and     0x3F
        or      0x80
        ld      d,a
        ld      bc,(chunk_size)
        ldir
        call    advance_source
        call    advance_destination
        ld      hl,(remaining)
        ld      de,(chunk_size)
        or      a
        sbc     hl,de
        ld      (remaining),hl
        jr      copy_initial_loop

; Copy all 8K bank blocks contiguously into the one allocated 16K storage
; segment.  The runtime selector handlers expose either half of this segment
; through the KSS 8K windows at 8000H and A000H.
copy_8k_banks:
        ld      a,(bank_count)
        or      a
        ret     z
        ld      b,a
        ld      hl,0
        ld      de,0x2000
copy_8k_size_loop:
        add     hl,de
        djnz    copy_8k_size_loop
        ld      (remaining),hl
        ld      hl,(source_position)
        ld      (bank_source_start),hl
        ld      a,(source_position+2)
        ld      (bank_source_start+2),a
        ld      hl,0x8000
        ld      (destination),hl
        ld      a,(RUNTIME_KSS_TABLE)
        out     (MAPPER_PAGE2),a
copy_8k_loop:
        ld      bc,(remaining)
        ld      a,b
        or      c
        jr      z,copy_8k_done
        call    source_boundary
        call    cap_bc_hl
        call    destination_boundary
        call    cap_bc_hl
        ld      (chunk_size),bc
        call    map_source_page1
        ; The whole F-1 8K bank set fits in one allocated 16K mapper
        ; segment.  Keep page 2 on that segment; map_destination_page2
        ; would incorrectly switch it back to the ordinary main-RAM table.
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,(destination)
        ld      a,d
        and     0x3F
        or      0x80
        ld      d,a
        ld      bc,(chunk_size)
        ldir
        call    advance_source
        call    advance_destination
        ld      hl,(remaining)
        ld      de,(chunk_size)
        or      a
        sbc     hl,de
        ld      (remaining),hl
        jr      copy_8k_loop
copy_8k_done:
        ld      hl,(bank_source_start)
        ld      (source_position),hl
        ld      a,(bank_source_start+2)
        ld      (source_position+2),a
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        or      a
        ret

; Copy all 16K banked blocks in reverse order.  Reverse order is important
; when KSS bank_offset overlaps the raw-file staging banks: it prevents a
; destination block from destroying a later source block.
copy_16k_banks:
        ld      a,(bank_count)
        or      a
        ret     z
        dec     a
        ld      (bank_index),a
        ld      hl,(source_position)
        ld      (extra_position),hl
        ld      a,(source_position+2)
        ld      (extra_position+2),a
        ld      a,(bank_count)
        ld      b,a
copy_16k_setup_position:
        dec     b
        jr      z,copy_16k_position_ready
        ld      hl,(extra_position)
        ld      de,0x4000
        add     hl,de
        ld      (extra_position),hl
        ld      a,(extra_position+2)
        adc     a,0
        ld      (extra_position+2),a
        jr      copy_16k_setup_position
copy_16k_position_ready:
        ld      hl,(extra_position)
        ld      (source_position),hl
        ld      a,(extra_position+2)
        ld      (source_position+2),a
copy_16k_loop:
        call    copy_explicit_bank
        ld      a,(bank_index)
        or      a
        ret     z
        dec     a
        ld      (bank_index),a
        ld      hl,(source_position)
        ld      de,0x4000
        or      a
        sbc     hl,de
        ld      (source_position),hl
        ld      a,(source_position+2)
        sbc     a,0
        ld      (source_position+2),a
        jr      copy_16k_loop

; Copy one 16K block from source_position into the mapper bank selected by
; bank_offset + bank_index.
copy_explicit_bank:
        ld      hl,(source_position)
        ld      (bank_source_start),hl
        ld      a,(source_position+2)
        ld      (bank_source_start+2),a
        ld      hl,0x4000
        ld      (remaining),hl
copy_explicit_loop:
        ld      bc,(remaining)
        call    source_boundary
        call    cap_bc_hl
        ld      (chunk_size),bc
        call    map_source_page1
        ld      a,(bank_index)
        ld      e,a
        ld      d,0
        ld      hl,RUNTIME_KSS_TABLE
        add     hl,de
        ld      a,(hl)
        out     (MAPPER_PAGE2),a
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,0x8000
        ld      bc,(chunk_size)
        ldir
        call    advance_source
        ld      hl,(remaining)
        ld      de,(chunk_size)
        or      a
        sbc     hl,de
        ld      (remaining),hl
        ld      a,h
        or      l
        jr      nz,copy_explicit_loop
        ld      hl,(bank_source_start)
        ld      (source_position),hl
        ld      a,(bank_source_start+2)
        ld      (source_position+2),a
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        or      a
        ret

; HL = number of bytes available before source page boundary.
source_boundary:
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        ld      h,a
        ld      de,0x4000
        ex      de,hl
        or      a
        sbc     hl,de
        ret

destination_boundary:
        ld      hl,(destination)
        ld      a,h
        and     0x3F
        ld      h,a
        ld      de,0x4000
        ex      de,hl
        or      a
        sbc     hl,de
        ret

; BC = min(BC, HL).
cap_bc_hl:
        ld      a,b
        cp      h
        jr      c,cap_done
        jr      nz,cap_set
        ld      a,c
        cp      l
        jr      c,cap_done
        ret     z
cap_set:
        ld      b,h
        ld      c,l
cap_done:
        ret

advance_source:
        ld      hl,(source_position)
        ld      de,(chunk_size)
        add     hl,de
        ld      (source_position),hl
        ld      a,(source_position+2)
        adc     a,0
        ld      (source_position+2),a
        ret

advance_destination:
        ld      hl,(destination)
        ld      de,(chunk_size)
        add     hl,de
        ld      (destination),hl
        ret

map_source_page1:
        ; source_position >> 14, looked up in the dynamic stage table.
        ld      a,(source_position+1)
        and     0xC0
        rrca
        rrca
        rrca
        rrca
        rrca
        rrca
        ld      c,a
        ld      a,(source_position+2)
        add     a,a
        add     a,a
        add     a,c
        ld      e,a
        ld      d,0
        ld      hl,RUNTIME_STAGE_TABLE
        add     hl,de
        ld      a,(hl)
        out     (MAPPER_PAGE1),a
        ret

map_destination_page2:
        ld      a,(destination+1)
        and     0xC0
        rrca
        rrca
        rrca
        rrca
        rrca
        rrca
        ld      c,a
        ld      e,c
        ld      d,0
        ld      hl,RUNTIME_MAIN_TABLE
        add     hl,de
        ld      a,(hl)
        out     (MAPPER_PAGE2),a
        ret

install_page0_stubs:
        ; Page 0 was switched to RAM directly by map_ram_pages.  Access the
        ; same physical segment through page 2 and install only the small
        ; BIOS-compatible surface required by the KSS runtime.
        ld      a,(RUNTIME_MAIN_TABLE+0)
        out     (MAPPER_PAGE2),a

        ld      hl,page0_wrtpsg
        ld      de,0x8001
        ld      bc,page0_wrtpsg_end-page0_wrtpsg
        ldir
        ld      hl,page0_rdpsg
        ld      de,0x8009
        ld      bc,page0_rdpsg_end-page0_rdpsg
        ldir

        ; RST 28H/30H are two-byte replacements for the original three-byte
        ; KSS 8K selector stores.  RST 30H also handles patched OUT (FE),A
        ; writes for 16K KSS images.
        ld      a,0xC3
        ld      (0x8024),a
        ld      hl,CUSTOM_ENASLT
        ld      (0x8025),hl
        ld      a,0xAF
        ld      (0x8138),a
        ld      a,0xC9
        ld      (0x8139),a
        ld      a,0xC3
        ld      (0x8028),a
        ld      hl,BANK1_HANDLER
        ld      (0x8029),hl
        ld      a,0xC3
        ld      (0x8030),a
        ld      hl,BANK0_HANDLER
        ld      (0x8031),hl
        ld      a,0xC3
        ld      (0x8038),a
        ld      hl,H_TIMI
        ld      (0x8039),hl
        ld      a,0xC3
        ld      (0x8093),a
        ld      a,0x01
        ld      (0x8094),a
        xor     a
        ld      (0x8095),a
        ld      a,0xC3
        ld      (0x8096),a
        ld      a,0x09
        ld      (0x8097),a
        xor     a
        ld      (0x8098),a
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        ret

install_runtime_stubs:
        ld      a,(RUNTIME_MAIN_TABLE+3)
        out     (MAPPER_PAGE2),a
        ; Keep the physical KSS segment table in the RAM page where the
        ; handlers remain visible after page 3 is switched away from player.
        ld      hl,RUNTIME_KSS_TABLE
        ld      de,0xB300
        ld      bc,32
        ldir
        ; Return stub at F380H, viewed through bank 0 at B380H.
        ld      hl,return_stub
        ld      de,0xB380
        ld      bc,return_stub_end-return_stub
        ldir
        ; The wrapper at F3A0H runs PLAY with KSS RAM visible and then
        ; mirrors the SCC register shadow to the real SCC cartridge.
        ld      hl,play_wrapper
        ld      de,0xB3A0
        ld      bc,play_wrapper_end-play_wrapper
        ldir
        ; The KSS image is allowed to use the ordinary RAM work area.  Put
        ; the values needed by the interrupt path directly in the copied
        ; instructions instead of leaving them in F370H..F37FH.
        ld      hl,(play_address)
        ld      (0xB3A0+(play_target_immediate-play_wrapper)+1),hl
        ld      a,(ram_slot)
        ld      (0xB3A0+(ram_slot_immediate-play_wrapper)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+2)
        ld      (0xB3A0+(page2_restore_immediate-play_wrapper)+1),a
        ld      hl,bank0_handler
        ld      de,BANK0_HANDLER-0x4000
        ld      bc,bank0_handler_end-bank0_handler
        ldir
        ld      a,(bank_mode)
        ld      (BANK0_HANDLER-0x4000+(bank0_mode_immediate-bank0_handler)+1),a
        ld      a,(bank_offset)
        ld      (BANK0_HANDLER-0x4000+(bank0_offset_immediate-bank0_handler)+1),a
        ld      (BANK0_HANDLER-0x4000+(bank0_16k_offset_immediate-bank0_handler)+1),a
        ld      a,(bank_count)
        ld      (BANK0_HANDLER-0x4000+(bank0_count_immediate-bank0_handler)+1),a
        ld      (BANK0_HANDLER-0x4000+(bank0_16k_count_immediate-bank0_handler)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+2)
        ld      (BANK0_HANDLER-0x4000+(bank0_main2_immediate-bank0_handler)+1),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (BANK0_HANDLER-0x4000+(bank0_storage_immediate-bank0_handler)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+1)
        ld      (BANK0_HANDLER-0x4000+(bank0_page1_immediate-bank0_handler)+1),a
        ld      hl,bank1_handler
        ld      de,BANK1_HANDLER-0x4000
        ld      bc,bank1_handler_end-bank1_handler
        ldir
        ld      a,(bank_offset)
        ld      (BANK1_HANDLER-0x4000+(bank1_offset_immediate-bank1_handler)+1),a
        ld      a,(bank_count)
        ld      (BANK1_HANDLER-0x4000+(bank1_count_immediate-bank1_handler)+1),a
        ld      a,(bank_mode)
        ld      (BANK1_HANDLER-0x4000+(bank1_mode_immediate-bank1_handler)+1),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (BANK1_HANDLER-0x4000+(bank1_storage_immediate-bank1_handler)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+1)
        ld      (BANK1_HANDLER-0x4000+(bank1_page1_immediate-bank1_handler)+1),a
        ld      hl,custom_enaslt
        ld      de,CUSTOM_ENASLT-0x4000
        ld      bc,custom_enaslt_end-custom_enaslt
        ldir
        ; H.TIMI at FD9FH: JP PLAY_WRAPPER, EI, RET.
        ld      a,0xC3
        ld      (0xBD9F),a
        ld      hl,PLAY_WRAPPER
        ld      (0xBDA0),hl
        ld      a,0xFB
        ld      (0xBDA2),a
        ld      a,0xC9
        ld      (0xBDA3),a
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        ret

install_init_trampoline:
        ld      a,(RUNTIME_MAIN_TABLE+3)
        out     (MAPPER_PAGE2),a
        ld      hl,init_trampoline
        ld      de,0x8000+(init_switch_next-0xC000)
        ld      bc,init_trampoline_end-init_trampoline
        ldir
        ld      a,(song_number)
        ld      (0x8000+(init_song_immediate-init_trampoline)+1+(init_switch_next-0xC000)),a
        ld      hl,(init_address)
        ld      (0x8000+(init_target_immediate-init_trampoline)+1+(init_switch_next-0xC000)),hl
        ld      a,(RUNTIME_BASIC_P2)
        out     (MAPPER_PAGE2),a
        ret

map_ram_pages:
        ld      a,(BIOS_RAMAD1)
        ld      (ram_slot),a
        ld      h,0x40
        call    BIOS_ENASLT
        ld      a,(ram_slot)
        ld      h,0x80
        call    BIOS_ENASLT
        ld      a,(ram_slot)
        ld      h,0xC0
        call    BIOS_ENASLT
        ; Switch page 0 by writing the primary/secondary slot registers
        ; directly.  BIOS ENASLT cannot be used after page 3 is RAM because
        ; its jump-table implementation lives in the BIOS page-3 image.
map_ram_page0:
        ld      a,(ram_slot)
        ld      b,a
        bit     7,b
        jr      z,map_ram_page0_primary
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

print_string:
        ld      a,(hl)
        or      a
        ret     z
        call    BIOS_CHPUT
        inc     hl
        jr      print_string

page0_wrtpsg:
        defb    0xD3,0xA0,0xF5,0x7B,0xD3,0xA1,0xF1,0xC9
page0_wrtpsg_end:
page0_rdpsg:
        defb    0xD3,0xA0,0xDB,0xA2,0xC9
page0_rdpsg_end:

return_stub:
        im      1
return_stub_halt:
        call    PLAY_WRAPPER
        jr      return_stub_halt
return_stub_end:

play_wrapper:
        ; Call PLAY while page 2 still contains the current KSS bank.
play_target_immediate:
        ld      hl,0
        ld      de,PLAY_WRAPPER+(play_after-play_wrapper)
        push    de
        jp      (hl)
play_after:
        ; The reference KSS VM starts the next PLAY with the registers left
        ; by the previous PLAY.  Preserve them while copying SCC shadows and
        ; switching the temporary cartridge slot.
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
        ; PLAY wrote SCC registers into the RAM shadow at 9800H.  Copy it
        ; before temporarily selecting the real SCC cartridge.
        ld      hl,0x9800
        ld      de,SCC_SHADOW
        ld      bc,0x0100
        ldir
        ld      hl,0xB800
        ld      de,SCC_PLUS_SHADOW
        ld      bc,0x0100
        ldir
        ld      a,SCC_SLOT
        ld      h,0x80
        call    BIOS_ENASLT
        ld      hl,SCC_SHADOW
        ld      de,0x9800
        ld      bc,0x0100
        ldir
        ld      hl,SCC_PLUS_SHADOW
        ld      de,0xB800
        ld      bc,0x0100
        ldir
        ; Restore page 2 to the RAM slot and its current KSS/main segment.
ram_slot_immediate:
        ld      a,0
        ld      h,0x80
        call    BIOS_ENASLT
page2_restore_immediate:
        ld      a,0
        out     (MAPPER_PAGE2),a
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
play_wrapper_end:

; These handlers are copied to fixed addresses in page 3.  Their branches are
; relative, while all dynamic state references use the fixed page-3 RAM
; addresses above; therefore copying them preserves their behavior.
bank0_handler:
        push    af
        push    bc
        push    de
        push    hl
        ld      b,a
bank0_mode_immediate:
        ld      a,0
        and     0x80
        jr      nz,bank0_handler_8k

        ; 16K mapper: translate the KSS bank number to the allocated mapper
        ; segment and select it in the real page-2 mapper.
        ld      a,b
        ld      c,a
bank0_16k_offset_immediate:
        ld      a,0
        ld      d,a
        ld      a,c
        sub     d
        jr      c,bank0_handler_16k_invalid
        ld      c,a
bank0_16k_count_immediate:
        ld      a,0
        cp      c
        jr      c,bank0_handler_16k_invalid
        jr      z,bank0_handler_16k_invalid
        ld      hl,KSS_TABLE_SAVED
        ld      e,c
        ld      d,0
        add     hl,de
        ld      a,(hl)
        ld      (PAGE2_RESTORE_SAVED),a
        out     (MAPPER_PAGE2),a
        jr      bank0_handler_done
bank0_handler_16k_invalid:
bank0_main2_immediate:
        ld      a,0
        ld      (PAGE2_RESTORE_SAVED),a
        out     (MAPPER_PAGE2),a
        jr      bank0_handler_done

bank0_handler_8k:
        ld      a,b
        ld      c,a
bank0_offset_immediate:
        ld      a,0
        ld      d,a
        ld      a,c
        sub     d
        jr      c,bank0_handler_8k_invalid
        ld      c,a
bank0_count_immediate:
        ld      a,0
        cp      c
        jr      c,bank0_handler_8k_invalid
        jr      z,bank0_handler_8k_invalid
bank0_storage_immediate:
        ld      a,0
        out     (MAPPER_PAGE1),a
        ld      a,c
        or      a
        jr      z,bank0_handler_8k_source0
        ld      hl,0x6000
        jr      bank0_handler_8k_source_ready
bank0_handler_8k_source0:
        ld      hl,0x4000
bank0_handler_8k_source_ready:
        ld      de,0x8000
        ld      bc,0x2000
        ldir
bank0_page1_immediate:
        ld      a,0
        out     (MAPPER_PAGE1),a
        jr      bank0_handler_done
bank0_handler_8k_invalid:
        ld      hl,0x8000
        ld      de,0x2000
        ld      a,0xFF
bank0_handler_8k_fill:
        ld      (hl),a
        inc     hl
        dec     de
        ld      a,d
        or      e
        jr      nz,bank0_handler_8k_fill
bank0_handler_done:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
bank0_handler_end:

bank1_handler:
        push    af
        push    bc
        push    de
        push    hl
        ld      b,a
        ld      a,b
        ld      c,a
bank1_offset_immediate:
        ld      a,0
        ld      d,a
        ld      a,c
        sub     d
        jr      c,bank1_handler_invalid
        ld      c,a
bank1_count_immediate:
        ld      a,0
        cp      c
        jr      c,bank1_handler_invalid
        jr      z,bank1_handler_invalid
bank1_mode_immediate:
        ld      a,0
        and     0x80
        jr      z,bank1_handler_invalid
bank1_storage_immediate:
        ld      a,0
        out     (MAPPER_PAGE1),a
        ld      a,c
        or      a
        jr      z,bank1_handler_source0
        ld      hl,0x6000
        jr      bank1_handler_source_ready
bank1_handler_source0:
        ld      hl,0x4000
bank1_handler_source_ready:
        ld      de,0xA000
        ld      bc,0x2000
        ldir
bank1_page1_immediate:
        ld      a,0
        out     (MAPPER_PAGE1),a
        jr      bank1_handler_done
bank1_handler_invalid:
        ld      hl,0xA000
        ld      de,0x2000
        ld      a,0xFF
bank1_handler_fill:
        ld      (hl),a
        inc     hl
        dec     de
        ld      a,d
        or      e
        jr      nz,bank1_handler_fill
bank1_handler_done:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
bank1_handler_end:

; Local replacement for BIOS ENASLT.  The runtime only needs page 2, so this
; routine updates the page-2 primary and secondary slot fields directly and
; remains callable after page 3 has become ordinary RAM.
custom_enaslt:
        push    af
        push    bc
        push    de
        push    hl
        ld      b,a
        bit     7,b
        jr      z,custom_enaslt_primary
        ld      a,(0xFFFF)
        cpl
        and     0xCF
        ld      c,a
        ld      a,b
        and     0xC0
        rrca
        rrca
        or      c
        ld      (0xFFFF),a
custom_enaslt_primary:
        in      a,(0xA8)
        and     0xCF
        ld      c,a
        ld      a,b
        and     0x03
        rla
        rla
        rla
        rla
        or      c
        out     (0xA8),a
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
custom_enaslt_end:

init_trampoline:
        ld      sp,STACK_TOP
        ld      hl,RETURN_STUB
        push    hl
init_song_immediate:
        ld      a,0
init_target_immediate:
        ld      hl,0
        jp      (hl)
init_trampoline_end:

; The H.TIMI vector is patched after the image copy because the image may
; legitimately include the BIOS work area.  This table is copied through the
; page-2 view of main mapper bank 0.
msg_format:
        defm    13,10,"NOT A KSS/KSSX FILE",13,10,0
msg_file:
        defm    13,10,"TRUNCATED KSS FILE",13,10,0
msg_8k:
        defm    13,10,"8K BANKED KSS NEEDS KSSPLAY.COM",13,10,0
msg_bank:
        defm    13,10,"KSS BANK RANGE NEEDS MORE MAPPER SPACE",13,10,0

header:         defs    0x20,0
file_size:      defs    3,0
load_address:   defw    0
remaining:      defw    0
init_address:   defw    0
play_address:   defw    0
source_position:defs    3,0
destination:    defw    0
extra_position: defs    3,0
bank_source_start: defs 3,0
chunk_size:     defw    0
check_position:  defs    3,0
stage_bank:     defb    STAGE_BANK
stage_offset:   defw    0
field_source:   defw    0
field_remaining:defw    0
bank_offset:    defb    0
bank_count:     defb    0
bank_index:     defb    0
bank_mode:      defb    0
extra_size:     defb    0
ram_slot:       defb    0
song_number:    defb    0
page3_segment:  defb    0

; The DOS2 loader fills this block after copying the player to C000H.  It is
; copied along with the player when start_player relocates the code.
        defs    RUNTIME_CONFIG-$,0
runtime_config:
        defs    RUNTIME_CONFIG_END-RUNTIME_CONFIG,0

player_end:
