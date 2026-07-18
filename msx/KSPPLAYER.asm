; MSX-DOS2 KSP player bootstrap.
;
; Supports generic Konami KCPX/KCPZ payloads and compact KSP1 MoonSound
; ENGN/SONG resources. The DOS2 front-end stages the file and supplies all
; dynamically allocated physical mapper segment IDs. The fixed parser and
; materializers are assembled in the free page-3 TPA at C000H.

        include 'PLAYER_LAYOUT.inc'

start:
        di
        ; The DOS2 front-end already queried EXTBIO and stored PUT_P0/P1/P2
        ; in the handoff block. Do not re-enter EXTBIO after taking over the
        ; fixed page-3 TPA; some DOS2 builds remap that page during EXTBIO.
        ; The DOS2 loader already captured a safe TPA stack top. Do not
        ; reread 0006H after ALL_SEG/BDOS activity; some DOS2 setups change
        ; that word while the loader is handing control over.
        defs    6,0
        call    install_dispatches
        ; KSPPLAY.COM always supplies the DOS2 mapper-call handoff. Keep the
        ; original nine-byte footprint so the page-1 runtime entry remains at
        ; the fixed address used by the bootstrap.
        jp      start_player
        defs    6,0

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
        call    PUT_P2_DISPATCH
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
        call    PUT_P2_DISPATCH
        ei
        xor     a
        ret

; Entry used for USR(0) after the file has been staged.
start_player:
        di
        ld      sp,(RUNTIME_STACK_TOP)
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
        ld      a,(RUNTIME_INPUT_FORMAT)
        cp      2
        jp      z,ksp_runtime_path
        ; Generic KCPX/KCPZ materialization uses page 1 as its destination,
        ; leaves page 0/page 3 in their DOS2 mappings and reserves page 2 for
        ; the real SCC slot. No raw-KSS fallback is supported.
        ld      a,(kcp_format)
        or      a
        jp      nz,kcpx_runtime_path
        jp      format_error

format_error:
        ld      hl,msg_format
        jr      report_error
file_error:
        ld      hl,msg_file
        jr      report_error
unsupported_bank:
        ld      hl,msg_bank
        jr      report_error
layout_error:
        ld      hl,msg_layout
report_error:
        ; Direct BIOS text calls are unsafe under DOS2 page-0 mappings. Restore
        ; the process mapper layout and return; the loader already reported
        ; format errors before transferring control whenever possible.
        di
        call    kcpz_select_ram_page2
        call    stop_runtime
        ld      sp,(RUNTIME_STACK_TOP)
        ei
        ld      bc,0
        jp      0x0005

; Read the first 32 bytes from staged mapper bank 16 into the relocated
; player's private header buffer.
read_header:
        ; Read through page 1.  This is important for KCPX: page 2 must
        ; remain available for the SCC cartridge once playback starts.
        ld      a,(RUNTIME_STAGE_TABLE)
        call    PUT_P1_DISPATCH
        ld      hl,0x4000
        ld      de,header
        ld      bc,0x20
        ldir
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH

        ld      a,(header)
        cp      'K'
        jp      nz,read_header_bad
        ld      a,(header+1)
        cp      'S'
        jp      nz,read_header_bad
        ld      a,(header+2)
        cp      'C'
        jr      z,read_header_kscc
        cp      'S'
        jp      nz,read_header_bad
        ld      a,(header+3)
        cp      'C'
        jr      z,read_header_kscc
        cp      'X'
        jp      nz,read_header_bad
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
        call    probe_kcpx
        ld      hl,(load_address)
        ld      (destination),hl
        or      a
        ret
read_header_bad:
        scf
        ret

; Bank data is assigned dynamically by the DOS2 loader.
validate_bank_range:
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

; Clear the KSS main-RAM pages that are available to the engine.  Main
; segment 0 deliberately remains untouched because it retains the page-0
; BIOS-compatible environment. The fixed page-3 TPA segment is also excluded.
clear_main_ram:
        ld      a,1
        ld      (bank_index),a
clear_main_ram_loop:
        ld      a,(bank_index)
        cp      3
        jr      z,clear_main_ram_done
        ld      e,a
        ld      d,0
        ld      hl,RUNTIME_MAIN_TABLE
        add     hl,de
        ld      a,(hl)
        call    PUT_P2_DISPATCH
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
        jr      clear_main_ram_loop
clear_main_ram_done:
        ld      a,(RUNTIME_BASIC_P2)
        call    PUT_P2_DISPATCH
        ret

copy_initial_image:
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
        call    PUT_P2_DISPATCH
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
        call    PUT_P2_DISPATCH
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
        call    PUT_P2_DISPATCH
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
        call    PUT_P2_DISPATCH
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
        call    PUT_P1_DISPATCH
        di
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
        call    PUT_P2_DISPATCH
        ret

install_page0_stubs:
        ; Page 0 was switched to RAM by map_ram_pages.  PUT_P0 selects the
        ; original page-0 segment captured by DOS2 before we modify the
        ; user-reserved RST 28H vector.
        ld      a,(RUNTIME_MAIN_TABLE+0)
        call    PUT_P0_DISPATCH
        ld      hl,0x0028
        ld      de,RST28_SAVED
        ld      bc,4
        ldir
        ld      a,0xC3
        ld      (0x0028),a
        ld      hl,BANK0_HANDLER
        ld      (0x0029),hl
        ld      a,1
        ld      (RST28_INSTALLED),a

        ; Access the same physical page-0 segment through page 2 and install
        ; only the small BIOS-compatible surface required by the KSS runtime.
        ld      a,(RUNTIME_MAIN_TABLE+0)
        call    PUT_P2_DISPATCH

        ld      hl,page0_wrtpsg
        ld      de,0x8001
        ld      bc,page0_wrtpsg_end-page0_wrtpsg
        ldir
        ld      hl,page0_rdpsg
        ld      de,0x8009
        ld      bc,page0_rdpsg_end-page0_rdpsg
        ldir

        ; 8024H is the engine's ENASLT gateway.  Because this page-2 view is
        ; the same physical segment as page 0, 8028H and 8030H alias the real
        ; RST 28H/RST 30H vectors. Keep the route mode-dependent: 16K
        ; engines use RST 28H, while the known 8K adapter keeps RST 28H
        ; for page-3/8K data and RST 30H for page-2 data.
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
        ld      a,(bank_mode)
        and     0x80
        jr      nz,install_rst28_bank1
        ld      hl,BANK0_HANDLER
        jr      install_rst28_target
install_rst28_bank1:
        ld      hl,BANK1_HANDLER
install_rst28_target:
        ld      (0x8029),hl
        ld      a,0xC3
        ld      (0x8030),a
        ld      hl,BANK0_HANDLER
        ld      (0x8031),hl
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
        call    PUT_P2_DISPATCH
        ret

install_runtime_stubs:
        ; All destinations below are in the fixed page-3 TPA.  No helper is
        ; copied through page 2 and page 3 is never remapped.
        ld      hl,RUNTIME_KSS_TABLE
        ld      de,KSS_TABLE_SAVED
        ld      bc,32
        ldir
        ld      hl,return_stub
        ld      de,RETURN_STUB
        ld      bc,return_stub_end-return_stub
        ldir
        ; The wrapper runs PLAY with KSS RAM visible and then
        ; mirrors the SCC register shadow to the real SCC cartridge.
        ld      hl,play_wrapper
        ld      de,PLAY_WRAPPER
        ld      bc,play_wrapper_end-play_wrapper
        ldir
        ; Put the values needed by the interrupt path directly in the copied
        ; instructions.
        ld      hl,(play_address)
        ld      (PLAY_WRAPPER+(play_target_immediate-play_wrapper)+1),hl
        ld      a,(ram_slot)
        ld      (PLAY_WRAPPER+(ram_slot_immediate-play_wrapper)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+2)
        ld      (PLAY_WRAPPER+(page2_restore_immediate-play_wrapper)+1),a
        ld      a,(RUNTIME_SCC_SLOT)
        ld      (PLAY_WRAPPER+(scc_slot_immediate-play_wrapper)+1),a
        ld      hl,bank0_handler
        ld      de,BANK0_HANDLER
        ld      bc,bank0_handler_end-bank0_handler
        ldir
        ld      a,(bank_mode)
        ld      (BANK0_HANDLER+(bank0_mode_immediate-bank0_handler)+1),a
        ld      a,(bank_offset)
        ld      (BANK0_HANDLER+(bank0_offset_immediate-bank0_handler)+1),a
        ld      (BANK0_HANDLER+(bank0_16k_offset_immediate-bank0_handler)+1),a
        ld      a,(bank_count)
        ld      (BANK0_HANDLER+(bank0_count_immediate-bank0_handler)+1),a
        ld      (BANK0_HANDLER+(bank0_16k_count_immediate-bank0_handler)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+2)
        ld      (BANK0_HANDLER+(bank0_main2_immediate-bank0_handler)+1),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (BANK0_HANDLER+(bank0_storage_immediate-bank0_handler)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+1)
        ld      (BANK0_HANDLER+(bank0_page1_immediate-bank0_handler)+1),a
        ld      hl,bank1_handler
        ld      de,BANK1_HANDLER
        ld      bc,bank1_handler_end-bank1_handler
        ldir
        ld      a,(bank_offset)
        ld      (BANK1_HANDLER+(bank1_offset_immediate-bank1_handler)+1),a
        ld      a,(bank_count)
        ld      (BANK1_HANDLER+(bank1_count_immediate-bank1_handler)+1),a
        ld      a,(bank_mode)
        ld      (BANK1_HANDLER+(bank1_mode_immediate-bank1_handler)+1),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (BANK1_HANDLER+(bank1_storage_immediate-bank1_handler)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+1)
        ld      (BANK1_HANDLER+(bank1_page1_immediate-bank1_handler)+1),a
        ld      hl,custom_enaslt
        ld      de,CUSTOM_ENASLT
        ld      bc,custom_enaslt_end-custom_enaslt
        ldir
        ld      hl,put_p0_dispatch
        ld      de,PUT_P0_DISPATCH
        ld      bc,put_p0_dispatch_end-put_p0_dispatch
        ldir
        ld      hl,put_p1_dispatch
        ld      de,PUT_P1_DISPATCH
        ld      bc,put_p1_dispatch_end-put_p1_dispatch
        ldir
        ld      hl,put_p2_dispatch
        ld      de,PUT_P2_DISPATCH
        ld      bc,put_p2_dispatch_end-put_p2_dispatch
        ldir
        ; Playback is clocked by the return stub's EI/HALT loop.  Leave the
        ; BIOS H.TIMI hook and the IM1 RST 38H chain untouched so DOS2 still
        ; acknowledges VDP interrupts and maintains its own timing state.
        xor     a
        ld      (HTIMI_INSTALLED),a
        ret

install_init_trampoline:
        ld      a,(song_number)
        ld      (init_song_immediate+1),a
        ld      hl,(init_address)
        ld      (init_target_immediate+1),hl
        ret

; Obtain the DOS2 mapper call table for both the COM front-end and the
; legacy BASIC entry.  The table layout is PUT_P0/GET_P0, PUT_P1/GET_P1,
; PUT_P2/GET_P2, PUT_P3/GET_P3, in three-byte call slots.
init_mapper_api:
        xor     a
        ld      d,4
        ld      e,2
        call    EXTBIO
        or      a
        ret     z
        push    hl
        ld      de,0x18
        add     hl,de
        ld      (RUNTIME_PUT_P0),hl
        pop     hl
        push    hl
        ld      de,0x1E
        add     hl,de
        ld      (RUNTIME_PUT_P1),hl
        pop     hl
        ld      de,0x24
        add     hl,de
        ld      (RUNTIME_PUT_P2),hl
        ret

; Copy the tiny indirect mapper gateways into the fixed page-3 resident
; area before any materialization code can request a new page.
install_dispatches:
        call    ensure_scc_slot
        ld      hl,put_p0_dispatch
        ld      de,PUT_P0_DISPATCH
        ld      bc,put_p0_dispatch_end-put_p0_dispatch
        ldir
        ld      hl,put_p1_dispatch
        ld      de,PUT_P1_DISPATCH
        ld      bc,put_p1_dispatch_end-put_p1_dispatch
        ldir
        ld      hl,put_p2_dispatch
        ld      de,PUT_P2_DISPATCH
        ld      bc,put_p2_dispatch_end-put_p2_dispatch
        ldir
        ret

; The DOS2 front-end writes the requested slot and magic before entering the
; bootstrap.  Disk BASIC has no command-line handoff, so initialize the
; documented default only when the magic is absent.
ensure_scc_slot:
        ld      a,(RUNTIME_SCC_SLOT_VALID)
        cp      SCC_SLOT_MAGIC
        ret     z
        ld      a,SCC_SLOT_DEFAULT
        ld      (RUNTIME_SCC_SLOT),a
        ld      a,SCC_SLOT_MAGIC
        ld      (RUNTIME_SCC_SLOT_VALID),a
        ret

map_ram_pages:
        ld      a,(BIOS_RAMAD1)
        ld      (ram_slot),a
        ld      h,0x40
        call    BIOS_ENASLT
        di
        ld      a,(ram_slot)
        ld      h,0x80
        call    BIOS_ENASLT
        di
        ; Page 0 is selected from the current RAMAD1 slot after the BIOS
        ; calls have completed; page 3 remains selected throughout.
        call    map_ram_page0
        ret
map_ram_page0:
        ld      a,(BIOS_RAMAD1)
        ld      b,a
        bit     7,b
        jr      z,map_ram_page0_primary
        ; FFFFH reads as the inverted secondary-slot register on MSX.
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
        ei
return_stub_halt:
        halt
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
scc_slot_immediate:
        ld      a,0
        ld      h,0x80
        call    CUSTOM_ENASLT
        ; Konami SCC cartridges require the enable write before the register
        ; window at 9800H is used.  Repeating it after every slot selection
        ; is harmless and also covers cartridges that reset their enable
        ; latch when the primary slot changes.
        ld      a,0x3F
        ld      (0x9000),a
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
        call    CUSTOM_ENASLT
page2_restore_immediate:
        ld      a,0
        call    PUT_P2_DISPATCH
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
        jr      bank0_handler_16k_done
bank0_handler_16k_invalid:
bank0_main2_immediate:
        ld      a,0
        ld      (PAGE2_RESTORE_SAVED),a
        jr      bank0_handler_16k_done

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
        call    PUT_P1_DISPATCH
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
        call    PUT_P1_DISPATCH
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
bank0_handler_16k_done:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ld      a,(PAGE2_RESTORE_SAVED)
        call    PUT_P2_DISPATCH
        ret
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
        call    PUT_P1_DISPATCH
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
        call    PUT_P1_DISPATCH
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

; ENASLT gateway used after page 0 has become RAM.  Directly editing FFFFH is
; only safe when the target expanded primary slot is already visible in page
; 3; otherwise FFFFH belongs to the wrong primary slot and remapping page 3
; would remove the executing code and stack.  Temporarily expose the BIOS in
; page 0, call the real BIOS ENASLT (which performs and restores its page-3
; trampoline), then restore only the original page-0 primary bits.
custom_enaslt:
        push    af
        push    bc
        push    de
        push    hl
        ld      b,a
        in      a,(0xA8)
        ld      (SLOT_A8_SAVED),a
        and     0xFC
        out     (0xA8),a
        ld      a,b
        call    BIOS_ENASLT
        ld      a,(SLOT_A8_SAVED)
        and     0x03
        ld      c,a
        in      a,(0xA8)
        and     0xFC
        or      c
        out     (0xA8),a
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
custom_enaslt_end:

; Indirect DOS2 PUT_P2 call used after the player has taken over. The
; routine itself lives in the resident page-3 player image, while DOS2
; retains ownership of the actual mapper operation.
put_p0_dispatch:
        ld      hl,(RUNTIME_PUT_P0)
        jp      (hl)
put_p0_dispatch_end:

put_p1_dispatch:
        ld      hl,(RUNTIME_PUT_P1)
        jp      (hl)
put_p1_dispatch_end:

put_p2_dispatch:
        ld      hl,(RUNTIME_PUT_P2)
        jp      (hl)
put_p2_dispatch_end:

; Restore the user restart/vector state and the mapper pages owned by the
; player. The final page-3 slot restoration belongs to the DOS2 front-end,
; because returning through code in page 3 after changing that slot is unsafe.
stop_runtime:
        ld      a,(RST28_INSTALLED)
        or      a
        jr      z,stop_runtime_no_rst28
        ld      a,(RUNTIME_MAIN_TABLE+0)
        call    PUT_P0_DISPATCH
        ld      hl,RST28_SAVED
        ld      de,0x0028
        ld      bc,4
        ldir
        xor     a
        ld      (RST28_INSTALLED),a
stop_runtime_no_rst28:
        ld      a,(HTIMI_INSTALLED)
        or      a
        jr      z,stop_runtime_no_htimi
        ld      hl,HTIMI_SAVED
        ld      de,H_TIMI
        ld      bc,5
        ldir
        xor     a
        ld      (HTIMI_INSTALLED),a
stop_runtime_no_htimi:
        ld      a,(RUNTIME_MAIN_TABLE+1)
        call    PUT_P1_DISPATCH
        ld      a,(RUNTIME_ORIGINAL_P2)
        call    PUT_P2_DISPATCH
        ret

init_trampoline:
        ld      sp,(RUNTIME_STACK_TOP)
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
        defm    13,10,"LEGACY 8K BANKED KSS IS NOT SUPPORTED",13,10,0
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
fast_8k:        defb    0
ram_slot:       defb    0
song_number:    defb    0
; The DOS2 loader fills this block after copying the player to C000H.  It is
; copied along with the player when start_player relocates the code.
        defs    RUNTIME_CONFIG-$,0
runtime_config:
        defs    RUNTIME_CONFIG_END-RUNTIME_CONFIG,0

; KCPX materializer state.  This follows the fixed handoff block so the
; original bootstrap still reaches RUNTIME_CONFIG at exactly C960H.
kcp_format:     defb    0
kcp_version:    defb    0
kcp_page_count: defb    0
kcp_track_count:defb    0
kcp_page_index: defb    0
kcp_selected_page: defb 0
kcp_materialized_page: defb 0xFF
kcp_original_song:  defb 0
kcp_current_song:   defb 0
kcp_key_latch:      defb 0
kcp_dest_segment:   defb 0
kcp_selected_segment:defb 0
kcp_temp_segment:   defb 0
kcp_value:      defb    0
kcp_scc_primary_bits:defb 0
kcp_engine_size:    defw 0
kcp_common_size:    defw 0
kcp_page_data_address:defw 0
kcp_engine_source:  defw 0
kcp_common_source:  defw 0
kcp_records_source: defw 0
kcp_data_source:defw 0
kcp_data_source_high:defb 0
kcpz_source_stage:defb 0
kcp_patch_source:defw 0
kcp_engine_compressed_size:defw 0
kcp_common_compressed_size:defw 0
kcp_data_compressed_size:defw 0
kcp_cursor:     defw    0
kcp_data_size:  defw    0
kcp_patch_count:defw    0
kcp_patch_left: defw    0
kcp_patch_offset:defw  0
kcp_patch_value:defw    0
kcp_dest_offset:defw    0
kcp_remaining:  defw    0
kcp_chunk_size: defw    0

; Compact resource-KSP materializer state (MoonSound engine type 1).
ksp_directory:      defw 0
ksp_entry_cursor:   defw 0
ksp_entry_count:    defb 0
ksp_found:          defb 0
ksp_entry_id:       defw 0
ksp_entry_offset:   defw 0
ksp_entry_packed:   defw 0
ksp_entry_unpacked: defw 0
ksp_entry_compression: defb 0
ksp_engine_offset:  defw 0
ksp_engine_packed:  defw 0
ksp_engine_size:    defw 0
ksp_engine_compression: defb 0
ksp_song_offset:    defw 0
ksp_song_packed:    defw 0
ksp_song_size:      defw 0
ksp_song_compression: defb 0
ksp_type:           defs 4,0
mwm_raw_cursor:     defw 0
mwm_compact_cursor: defw 0
mwm_patterns_left:  defb 0

; Kept after the fixed handoff block so the player can retain its C960H
; runtime configuration without growing into the resident page-3 helpers.
check_load_window:
        ld      hl,(load_address)
        ld      a,h
        cp      0x40
        jr      c,check_load_window_bad
        ld      de,(remaining)
        add     hl,de
        jr      c,check_load_window_bad
        ld      a,h
        cp      0xC0
        jr      nc,check_load_window_bad
        or      a
        ret
check_load_window_bad:
        scf
        ret
msg_layout:
        defm    13,10,"KSS LOAD IMAGE MUST STAY BELOW C000H",13,10,0

player_code_end:

; -------------------------------------------------------------------------
; Generic KCPX/KCPZ complete-page path. One selected logical page is
; materialized into page 1 while page 2 remains assigned to the SCC.

kcpx_runtime_path:
        call    kcpx_parse
        jp      c,format_error
        ld      a,0xFF
        ld      (kcp_materialized_page),a
        ld      a,(song_number)
        ld      (kcp_current_song),a
        ; A relaunched player may see the cursor key that requested the
        ; switch still held. Start latched and clear it on the first key-up.
        ld      a,1
        ld      (kcp_key_latch),a
        call    kcpx_get_song_mapping
        jp      c,format_error
        call    kcpx_materialize_dispatch
        jp      c,format_error

        ; Keep the KCPX parser/materializer resident for live track changes.
        ; The legacy runtime installer would overwrite it with bank handlers
        ; and SCC shadow buffers, none of which this complete-page path uses.
        ld      hl,kcpx_return_stub
        ld      de,RETURN_STUB
        ld      bc,kcpx_return_stub_end-kcpx_return_stub
        ldir
        xor     a
        ld      (HTIMI_INSTALLED),a

        ; Install a direct KCPX wrapper outside the retained materializer.
        ; BIOS/DOS can
        ; restore its normal mapper and slot layout while servicing HALT, so
        ; every PLAY must first reassert page 1 and the page-2 SCC slot.
        ;
        ; Mapper and slot helpers may alter registers that the KSS engine
        ; retains between ticks. Copy a wrapper that preserves the complete
        ; main/index register set around those helpers.
        ld      hl,kcpx_play_wrapper
        ld      de,PLAY_WRAPPER
        ld      bc,kcpx_play_wrapper_end-kcpx_play_wrapper
        ldir
        ld      a,(kcp_selected_segment)
        ld      (PLAY_WRAPPER+(kcpx_page1_segment_immediate-kcpx_play_wrapper)+1),a
        ld      a,(RUNTIME_SCC_SLOT)
        ld      (PLAY_WRAPPER+(kcpx_scc_slot_immediate-kcpx_play_wrapper)+1),a
        ld      hl,(play_address)
        ld      (PLAY_WRAPPER+(kcpx_play_target_immediate-kcpx_play_wrapper)+1),hl

        ; The OpenMSX/extb SCC is a non-expanded primary slot. Selecting it
        ; needs only the page-2 bits of port A8; BIOS ENASLT is unnecessary
        ; and its page-3 trampoline is unsafe for our resident TPA code.
        ld      a,(RUNTIME_SCC_SLOT)
        cp      4
        jp      nc,format_error
        add     a,a
        add     a,a
        add     a,a
        add     a,a
        ld      (kcp_scc_primary_bits),a
        ld      hl,CUSTOM_ENASLT
        ld      (hl),0xF5             ; PUSH AF
        inc     hl
        ld      (hl),0xDB             ; IN A,(A8H)
        inc     hl
        ld      (hl),0xA8
        inc     hl
        ld      (hl),0xE6             ; AND CFH: preserve pages 0,1,3
        inc     hl
        ld      (hl),0xCF
        inc     hl
        ld      (hl),0xF6             ; OR primary-slot page-2 bits
        inc     hl
        ld      a,(kcp_scc_primary_bits)
        ld      (hl),a
        inc     hl
        ld      (hl),0xD3             ; OUT (A8H),A
        inc     hl
        ld      (hl),0xA8
        inc     hl
        ld      (hl),0xF1             ; POP AF
        inc     hl
        ld      (hl),0xC9             ; RET

        ; Page 2 is the real SCC slot for the complete-page engine.  This
        ; call runs from fixed page 3 and therefore remains safe while the
        ; page-2 primary slot is changed.
        ld      a,(RUNTIME_SCC_SLOT)
        ld      h,0x80
        call    CUSTOM_ENASLT
        ; Enable the Konami SCC register window in the selected cartridge.
        ; KCPX keeps page 2 on this slot for the complete playback lifetime.
        ld      a,0x3F
        ld      (0x9000),a

        ld      a,(kcp_original_song)
        ld      (song_number),a
        call    install_init_trampoline
        ld      a,(kcp_selected_segment)
        call    PUT_P1_DISPATCH
        jp      init_trampoline

; The copied loop remains outside page 1, so it can replace the complete
; engine/music page while changing tracks.
kcpx_return_stub:
        im      1
        ei
kcpx_return_halt:
        halt
        call    PLAY_WRAPPER
        di
        ; Engines may retain useful Z80 state between PLAY calls. Keyboard
        ; scanning is player bookkeeping and must remain invisible to them.
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
        call    kcpx_poll_keyboard
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ei
        jr      kcpx_return_halt
kcpx_return_stub_end:

kcpx_play_wrapper:
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
kcpx_page1_segment_immediate:
        ld      a,0
        call    PUT_P1_DISPATCH
kcpx_scc_slot_immediate:
        ld      a,0
        ld      h,0x80
        call    CUSTOM_ENASLT
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
kcpx_play_target_immediate:
        ld      hl,0
        ld      de,PLAY_WRAPPER+(kcpx_play_return-kcpx_play_wrapper)
        push    de
        jp      (hl)
kcpx_play_return:
        ret
kcpx_play_wrapper_end:

kcpx_poll_keyboard:
        ; Poll the keyboard directly through the PPI. BIOS keyboard calls can
        ; invoke slot-dependent code while page 1 contains complete-page RAM.
        ; Escape is row 7 bit 2. Ctrl-C is row 6 bit 1 plus row 3 bit 0.
        ; Row 8 contains Right bit 0, Left bit 3 and Space bit 7.
        in      a,(0xAA)
        ld      e,a
        and     0xF0
        or      7
        out     (0xAA),a
        in      a,(0xA9)
        cpl
        bit     2,a
        jr      nz,kcpx_exit_key

        ld      a,e
        and     0xF0
        or      6
        out     (0xAA),a
        in      a,(0xA9)
        cpl
        bit     1,a
        jr      z,kcpx_poll_row8
        ld      a,e
        and     0xF0
        or      3
        out     (0xAA),a
        in      a,(0xA9)
        cpl
        bit     0,a
        jr      nz,kcpx_exit_key

kcpx_poll_row8:
        ld      a,e
        and     0xF0
        or      8
        out     (0xAA),a
        in      a,(0xA9)
        cpl
        and     0x89
        ld      b,a
        ld      a,e
        out     (0xAA),a
        ld      a,b
        or      a
        jr      nz,kcpx_key_pressed
        xor     a
        ld      (kcp_key_latch),a
        ret
kcpx_exit_key:
        ld      a,e
        out     (0xAA),a
        jp      kcpx_exit_to_dos
kcpx_key_pressed:
        ld      c,a
        ld      a,(kcp_key_latch)
        or      a
        ret     nz
        ld      a,1
        ld      (kcp_key_latch),a
        bit     0,c
        jr      nz,kcpx_next_song
        bit     3,c
        jr      nz,kcpx_previous_song
        bit     7,c
        ret     z
        ld      a,(kcp_current_song)
        jp      kcpx_switch_song

; Return from the complete-page player to COMMAND2.COM. Page 2 currently
; exposes the SCC cartridge, so first select mapper RAM there, then restore
; all mapper/vector state owned by the runtime. DOS2 TERM0 reclaims mapper
; segments allocated by this process.
kcpx_exit_to_dos:
        di
        call    kcpx_silence
        call    kcpz_select_ram_page2
        call    stop_runtime
        ld      sp,(RUNTIME_STACK_TOP)
        ei
        ld      bc,0
        jp      0x0005

kcpx_next_song:
        ld      a,(kcp_current_song)
        inc     a
        ld      c,a
        ld      a,(kcp_track_count)
        cp      c
        ld      a,c
        jr      nz,kcpx_switch_song
        xor     a
        jr      kcpx_switch_song

kcpx_previous_song:
        ld      a,(kcp_current_song)
        or      a
        jr      nz,kcpx_previous_decrement
        ld      a,(kcp_track_count)
kcpx_previous_decrement:
        dec     a

kcpx_switch_song:
        di
        ; Route every Konami switch through the fixed page-0 loader. It can
        ; print the cached title safely under DOS2; sparse packs additionally
        ; refill only missing compressed source segments.
        jp      kcpx_reload_sparse_song
kcpx_switch_song_resident_legacy:
        ld      (kcp_current_song),a
        ld      (song_number),a
        call    kcpx_silence
        call    kcpx_get_song_mapping
        jp      c,format_error
        call    kcpx_materialize_dispatch
        jp      c,format_error
        ld      a,(kcp_original_song)
        ld      (song_number),a
        call    install_init_trampoline
        ld      a,(kcp_selected_segment)
        call    PUT_P1_DISPATCH
        ld      a,(RUNTIME_SCC_SLOT)
        ld      h,0x80
        call    CUSTOM_ENASLT
        ld      a,0x3F
        ld      (0x9000),a
        jp      init_trampoline

; Large KCPZ archives keep only the active compressed overlay staged. Restore
; DOS2's mapper/slot state and return to the page-0 loader, which refills the
; sparse stage table and launches this bootstrap again for the requested song.
kcpx_reload_sparse_song:
        ld      (kcp_current_song),a
        call    kcpx_silence
        call    kcpz_select_ram_page2
        call    stop_runtime
        ld      sp,(RUNTIME_STACK_TOP)
        ld      a,(kcp_current_song)
        ld      hl,(RUNTIME_RELOAD_ENTRY)
        jp      (hl)

kcpx_silence:
        ; Some original drivers leave page 2 in their own slot layout. Select
        ; and enable the physical SCC before clearing it, otherwise these
        ; writes can hit mapper RAM while the real chip keeps ringing.
        ld      a,(RUNTIME_SCC_SLOT)
        ld      h,0x80
        call    CUSTOM_ENASLT
        ld      a,0x3F
        ld      (0x9000),a
        xor     a
        ld      hl,0x9880
        ld      b,16
kcpx_silence_scc:
        ld      (hl),a
        inc     hl
        djnz    kcpx_silence_scc
        ld      b,3
        ld      c,8
kcpx_silence_psg:
        ld      a,c
        out     (0xA0),a
        xor     a
        out     (0xA1),a
        inc     c
        djnz    kcpx_silence_psg
        ret

kcpx_parse:
        ; Header fields are little-endian and live at KCPX+04..0F.
        ld      hl,0x25
        call    kcpx_set_source
        call    kcpx_read_byte
        ld      (kcp_version),a
        cp      1
        jr      z,kcpx_version_ok
        cp      2
        jp      nz,kcpx_parse_bad
        ld      a,(kcp_format)
        cp      6
        jp      nz,kcpx_parse_bad
kcpx_version_ok:
        call    kcpx_read_byte
        or      a
        jp      z,kcpx_parse_bad
        cp      0x80
        jp      nc,kcpx_parse_bad
        ld      (kcp_page_count),a
        call    kcpx_read_byte
        ld      (kcp_track_count),a
        call    kcpx_read_byte

        ld      hl,0x29
        call    kcpx_set_source
        call    kcpx_read_word
        ld      (kcp_engine_size),hl
        call    kcpx_read_word
        ld      (kcp_common_size),hl
        call    kcpx_read_word
        ld      (kcp_page_data_address),hl
        call    kcpx_read_word

        jp      kcpx_parse_sources

; KCP_PARSE_BEGIN
kcpx_parse_sources:
        ; Generic KCP stores a complete 16K page at 4000H. The third word is
        ; the relocated workspace address and is intentionally pack-specific.
        ld      hl,(kcp_engine_size)
        ld      de,0x4000
        or      a
        sbc     hl,de
        jr      nz,kcpx_parse_bad
        ld      hl,(kcp_common_size)
        ld      de,0x4000
        or      a
        sbc     hl,de
        jr      nz,kcpx_parse_bad
        ld      a,(kcp_format)
        cp      6
        jr      z,kcpz_parse_sources
        ld      hl,0x231
        ld      (kcp_engine_source),hl
        ld      hl,0x4231
        ld      (kcp_records_source),hl
        or      a
        ret

kcpz_parse_sources:
        ; Template descriptor: compressed size and offset relative to KCPZ.
        ld      hl,0x231
        call    kcpx_set_source
        call    kcpx_read_word
        ld      (kcp_engine_compressed_size),hl
        call    kcpx_read_word
        ld      de,0x21
        add     hl,de
        ld      (kcp_engine_source),hl
        ld      hl,0x235
        ld      (kcp_records_source),hl
        or      a
        ret
; KCP_PARSE_END
kcpx_parse_bad:
        scf
        ret

; Map the source-positioned byte and return it in A.  The original page-1
; segment is restored before returning so every metadata read is isolated.
; The source position is advanced by one byte.
kcpx_read_byte:
        call    map_source_page1
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      a,(hl)
        ld      (kcp_value),a
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ld      hl,(source_position)
        inc     hl
        ld      (source_position),hl
        ld      a,h
        or      l
        jr      nz,kcpx_read_byte_no_carry
        ld      a,(source_position+2)
        inc     a
        ld      (source_position+2),a
kcpx_read_byte_no_carry:
        ld      a,(kcp_value)
        ret

kcpx_read_word:
        call    kcpx_read_byte
        push    af
        call    kcpx_read_byte
        ld      h,a
        pop     af
        ld      l,a
        ret

kcpx_set_source:
        ld      (source_position),hl
        xor     a
        ld      (source_position+2),a
        ret

; Translate the contiguous container song ID to the original engine selector
; and its logical complete-page index.
kcpx_get_song_mapping:
        ld      a,(song_number)
        ld      c,a
        ld      b,0
        ld      hl,0x31              ; KCPX + 10H
        add     hl,bc
        call    kcpx_set_source
        call    kcpx_read_byte
        cp      0xFF
        jr      z,kcpx_mapping_bad
        ld      (kcp_original_song),a

        ; kcpx_read_byte/map_source_page1 uses C internally. Reload the
        ; requested contiguous song ID before indexing the page table;
        ; otherwise every song accidentally materializes logical page 0.
        ld      a,(song_number)
        ld      c,a
        ld      b,0
        ld      hl,0x131             ; KCPX + 110H
        add     hl,bc
        call    kcpx_set_source
        call    kcpx_read_byte
        cp      0xFF
        jr      z,kcpx_mapping_bad
        ld      c,a
        ld      a,(kcp_page_count)
        cp      c
        jr      c,kcpx_mapping_bad
        jr      z,kcpx_mapping_bad
        ld      a,c
        ld      (kcp_selected_page),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (kcp_selected_segment),a
        or      a
        ret
kcpx_mapping_bad:
        scf
        ret

kcpx_materialize_dispatch:
        ld      a,(kcp_format)
        cp      5
        jp      z,kcpx_materialize_selected
        cp      6
        jp      z,kcpz_materialize_selected
        scf
        ret

; Shared compressed-source decoder for generic KCPZ.
kcpz_decompress_source:
        ; Temporarily replace SCC page 2 by mapper RAM and decode into the
        ; persistent page-1 destination. Compressed streams may cross staged
        ; 16K segment boundaries; kcpz_read_source_byte advances page 2.
        push    de
        call    kcpz_select_ram_page2
        call    kcpz_map_source_page2
        pop     de
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x80
        ld      h,a
        call    kcpz_zx0_decoder
        ret

kcpz_select_ram_page2:
        ; Select RAMAD1's primary slot in page 2 without calling ENASLT.
        ; This code runs in page 3, so preserve both page-3 primary bits and
        ; (for an expanded slot) page-3 secondary bits exactly.
        ld      a,(BIOS_RAMAD1)
        ld      b,a
        and     3
        rlca
        rlca
        rlca
        rlca
        ld      c,a
        in      a,(0xA8)
        and     0xCF
        or      c
        out     (0xA8),a
        bit     7,b
        ret     z

        ; FFFFH is accessible because the RAM primary slot is already the
        ; one visible in page 3. Reads are inverted; writes are not.
        ld      a,(0xFFFF)
        cpl
        and     0xCF
        ld      c,a
        ld      a,b
        and     0x0C
        rlca
        rlca
        or      c
        ld      (0xFFFF),a
        ret

kcpz_map_source_page2:
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
        ld      (kcpz_source_stage),a
        ld      hl,RUNTIME_STAGE_TABLE
        add     hl,de
        ld      a,(hl)
        call    PUT_P2_DISPATCH
        di
        ret

; Advance compressed input from C000H to the next independently allocated
; staging segment in page 2. Callers detect the boundary and preserve ZX0's
; accumulator/flags before entering here.
kcpz_advance_source_page2:
        push    bc
        push    de
        ld      a,(kcpz_source_stage)
        inc     a
        ld      (kcpz_source_stage),a
        ld      e,a
        ld      d,0
        ld      hl,RUNTIME_STAGE_TABLE
        add     hl,de
        ld      a,(hl)
        call    PUT_P2_DISPATCH
        pop     de
        ld      hl,0x8000
        pop     bc
        ret
; KCPZ_BOOTSTRAP_END

; KCP_MATERIALIZER_BEGIN
; Generic uncompressed complete page: copy the common 16K template, locate
; the selected variable-length overlay, then apply its offset/length records.
kcpx_materialize_selected:
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (kcp_selected_segment),a
        ld      (kcp_dest_segment),a
        ld      hl,(kcp_engine_source)
        call    kcpx_set_source
        xor     a
        ld      (kcp_dest_offset),a
        ld      (kcp_dest_offset+1),a
        ld      bc,0x4000
        call    kcpx_copy_range

        ld      hl,(kcp_records_source)
        ld      (kcp_cursor),hl
        ld      a,(kcp_selected_page)
        ld      (kcp_page_index),a
kcpx_overlay_scan:
        ld      hl,(kcp_cursor)
        call    kcpx_set_source
        call    kcpx_read_word
        ld      (kcp_data_size),hl
        ld      a,(kcp_page_index)
        or      a
        jr      z,kcpx_overlay_found
        ld      hl,(kcp_cursor)
        ld      de,2
        add     hl,de
        ld      de,(kcp_data_size)
        add     hl,de
        ld      (kcp_cursor),hl
        ld      a,(kcp_page_index)
        dec     a
        ld      (kcp_page_index),a
        jr      kcpx_overlay_scan

kcpx_overlay_found:
        ; Stream the sparse overlay through the fixed page-3 scratch buffer.
        ; kcp_patch_offset/value are reused as buffer cursor/bytes available.
        ld      hl,(kcp_data_size)
        ld      (kcp_patch_left),hl
        xor     a
        ld      (kcp_patch_value),a
        ld      (kcp_patch_value+1),a
        ld      a,(kcp_selected_segment)
        call    PUT_P1_DISPATCH
kcpx_overlay_loop:
        ld      hl,(kcp_patch_left)
        ld      a,h
        or      l
        jr      z,kcpx_overlay_done
        call    kcpx_buffer_read_word
        ret     c
        ld      (kcp_dest_offset),hl
        call    kcpx_buffer_read_word
        ret     c
        ld      (kcp_chunk_size),hl
        call    kcpx_buffer_copy_record
        ret     c
        jr      kcpx_overlay_loop
kcpx_overlay_done:
        ld      a,(kcp_selected_page)
        ld      (kcp_materialized_page),a
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        or      a
        ret

; Refill at most 1KB from staged storage. Page 1 is restored directly to the
; final engine segment, never to a second temporary mapper segment.
kcpx_buffer_fill:
        ld      hl,(kcp_patch_left)
        ld      a,h
        or      l
        jr      z,kcpx_buffer_error
        ld      b,h
        ld      c,l
        call    source_boundary
        call    cap_bc_hl
        ld      hl,KCPX_SCRATCH_SIZE
        call    cap_bc_hl
        ld      (chunk_size),bc
        call    map_source_page1
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,KCPX_SCRATCH
        ld      bc,(chunk_size)
        ldir
        ld      a,(kcp_selected_segment)
        call    PUT_P1_DISPATCH
        call    advance_source
        ld      hl,KCPX_SCRATCH
        ld      (kcp_patch_offset),hl
        ld      hl,(chunk_size)
        ld      (kcp_patch_value),hl
        or      a
        ret
kcpx_buffer_error:
        scf
        ret

kcpx_buffer_read_byte:
        ld      hl,(kcp_patch_value)
        ld      a,h
        or      l
        call    z,kcpx_buffer_fill
        ret     c
        ld      hl,(kcp_patch_offset)
        ld      a,(hl)
        ld      (kcp_value),a
        inc     hl
        ld      (kcp_patch_offset),hl
        ld      hl,(kcp_patch_value)
        dec     hl
        ld      (kcp_patch_value),hl
        ld      hl,(kcp_patch_left)
        dec     hl
        ld      (kcp_patch_left),hl
        ld      a,(kcp_value)
        or      a
        ret

kcpx_buffer_read_word:
        call    kcpx_buffer_read_byte
        ret     c
        push    af
        call    kcpx_buffer_read_byte
        jr      c,kcpx_buffer_word_error
        ld      h,a
        pop     af
        ld      l,a
        or      a
        ret
kcpx_buffer_word_error:
        pop     af
        scf
        ret

; Copy one record's data from the page-3 buffer into the final page-1 image.
; Records may span any number of scratch-buffer or staged-page boundaries.
kcpx_buffer_copy_record:
        ld      hl,(kcp_chunk_size)
        ld      a,h
        or      l
        ret     z
        ld      hl,(kcp_patch_value)
        ld      a,h
        or      l
        call    z,kcpx_buffer_fill
        ret     c
        ld      bc,(kcp_chunk_size)
        ld      hl,(kcp_patch_value)
        call    cap_bc_hl
        ld      (kcp_remaining),bc
        ld      hl,(kcp_patch_offset)
        ld      de,(kcp_dest_offset)
        push    hl
        ld      hl,0x4000
        add     hl,de
        ex      de,hl
        pop     hl
        ld      bc,(kcp_remaining)
        ldir
        ld      (kcp_patch_offset),hl
        ld      hl,(kcp_patch_value)
        ld      de,(kcp_remaining)
        or      a
        sbc     hl,de
        ld      (kcp_patch_value),hl
        ld      hl,(kcp_patch_left)
        or      a
        sbc     hl,de
        ld      (kcp_patch_left),hl
        ld      hl,(kcp_chunk_size)
        or      a
        sbc     hl,de
        ld      (kcp_chunk_size),hl
        ld      hl,(kcp_dest_offset)
        add     hl,de
        ld      (kcp_dest_offset),hl
        jr      kcpx_buffer_copy_record

; Generic compressed complete page. Decode the template into the persistent
; page-1 segment and the sparse overlay into a temporary allocated segment.
; The latter is then exposed through page 2 while its records patch page 1.
kcpz_materialize_selected:
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (kcp_selected_segment),a
        ld      (kcp_dest_segment),a
        ld      a,(RUNTIME_KSS_TABLE+1)
        ld      (kcp_temp_segment),a
        call    kcpz_read_selected_descriptor

        ld      a,(kcp_dest_segment)
        call    PUT_P1_DISPATCH
        xor     a
        ld      (0x4000),a
        ld      hl,0x4000
        ld      de,0x4001
        ld      bc,0x3FFF
        ldir
        ld      hl,(kcp_engine_source)
        call    kcpx_set_source
        ld      de,0x4000
        call    kcpz_decompress_source

        ld      a,(kcp_temp_segment)
        ld      (kcp_dest_segment),a
        call    PUT_P1_DISPATCH
        ld      hl,(kcp_data_source)
        call    kcpx_set_source
        ld      a,(kcp_data_source_high)
        ld      (source_position+2),a
        ld      de,0x4000
        call    kcpz_decompress_source

        ld      a,(kcp_selected_segment)
        ld      (kcp_dest_segment),a
        call    PUT_P1_DISPATCH
        call    kcpz_select_ram_page2
        ld      a,(kcp_temp_segment)
        call    PUT_P2_DISPATCH
        ld      hl,0x8000
        ld      de,(kcp_data_size)
        ld      (kcp_remaining),de
kcpz_overlay_loop:
        ld      de,(kcp_remaining)
        ld      a,d
        or      e
        jr      z,kcpz_overlay_done
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        inc     hl
        ld      (kcp_dest_offset),de
        ld      e,(hl)
        inc     hl
        ld      d,(hl)
        inc     hl
        ld      (kcp_chunk_size),de
        push    hl
        ld      hl,(kcp_dest_offset)
        ld      de,0x4000
        add     hl,de
        ex      de,hl
        pop     hl
        ld      bc,(kcp_chunk_size)
        ldir
        ; LDIR leaves HL at the next overlay record. Preserve that source
        ; pointer while updating the remaining encoded byte count; otherwise
        ; the next iteration reads its record header from the count itself.
        push    hl
        ld      hl,(kcp_remaining)
        ld      de,(kcp_chunk_size)
        or      a
        sbc     hl,de
        ld      de,4
        or      a
        sbc     hl,de
        ld      (kcp_remaining),hl
        pop     hl
        jr      kcpz_overlay_loop
kcpz_overlay_done:
        ld      a,(kcp_selected_page)
        ld      (kcp_materialized_page),a
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        or      a
        ret

kcpz_read_selected_descriptor:
        ld      a,(kcp_selected_page)
        ld      e,a
        ld      d,0
        ld      l,e
        ld      h,d
        add     hl,hl
        add     hl,de
        add     hl,hl
        ld      a,(kcp_version)
        cp      2
        jr      nz,kcpz_descriptor_stride_ready
        ; Version 2 records are eight bytes, not six.
        add     hl,de
        add     hl,de
kcpz_descriptor_stride_ready:
        ld      de,(kcp_records_source)
        add     hl,de
        call    kcpx_set_source
        call    kcpx_read_word
        ld      (kcp_data_size),hl
        call    kcpx_read_word
        ld      (kcp_data_compressed_size),hl
        call    kcpx_read_word
        push    hl
        xor     a
        ld      (kcp_data_source_high),a
        ld      a,(kcp_version)
        cp      2
        jr      nz,kcpz_descriptor_offset_read
        call    kcpx_read_word
        ld      a,h
        or      a
        jp      nz,kcpx_parse_bad
        ld      a,l
        ld      (kcp_data_source_high),a
kcpz_descriptor_offset_read:
        pop     hl
        ld      de,0x21
        add     hl,de
        ld      (kcp_data_source),hl
        jr      nc,kcpz_descriptor_source_ready
        ld      a,(kcp_data_source_high)
        inc     a
        ld      (kcp_data_source_high),a
kcpz_descriptor_source_ready:
        ret
; KCP_MATERIALIZER_END

 ; Copy a range from the staged file to the selected physical page-1 segment.
; The source position and destination offset are set by the caller, and BC
; contains the range length.  A 0400H scratch buffer avoids aliasing the two
; page-1 mappings.
kcpx_copy_range:
        ld      (kcp_remaining),bc
kcpx_copy_loop:
        ld      hl,(kcp_remaining)
        ld      a,h
        or      l
        jp      z,kcpx_copy_done
        ld      bc,(kcp_remaining)
        call    source_boundary
        call    cap_bc_hl
        ld      (kcp_chunk_size),bc
        ld      hl,(kcp_dest_offset)
        ld      a,h
        and     0x3F
        ld      h,a
        ld      de,0x4000
        ex      de,hl
        ld      bc,(kcp_chunk_size)
        call    cap_bc_hl
        ld      (kcp_chunk_size),bc
        ; Never use more than the fixed page-3 scratch buffer.
        ld      hl,KCPX_SCRATCH_SIZE
        ld      bc,(kcp_chunk_size)
        call    cap_bc_hl
        ld      (kcp_chunk_size),bc

        call    map_source_page1
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,KCPX_SCRATCH
        ld      bc,(kcp_chunk_size)
        ldir

        ld      a,(kcp_dest_segment)
        call    PUT_P1_DISPATCH
        ld      hl,(kcp_dest_offset)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ex      de,hl
        ld      hl,KCPX_SCRATCH
        ld      bc,(kcp_chunk_size)
        ldir

        ld      hl,(kcp_chunk_size)
        ld      (chunk_size),hl
        call    advance_source
        ld      hl,(kcp_dest_offset)
        ld      de,(kcp_chunk_size)
        add     hl,de
        ld      (kcp_dest_offset),hl
        ld      hl,(kcp_remaining)
        ld      de,(kcp_chunk_size)
        or      a
        sbc     hl,de
        ld      (kcp_remaining),hl
        jp      kcpx_copy_loop
kcpx_copy_done:
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ret

; Standard forward ZX0 decoder. HL is a compressed stream in staged page 2;
; DE is the destination in the persistent page-1 complete page.
; KCPZ_DECODER_BEGIN
kcpz_zx0_decoder:
        ld      bc,0xFFFF
        push    bc
        inc     bc
        ld      a,0x80
kcpz_zx0_literals:
        call    kcpz_zx0_elias
        ; LDIR cannot cross from staged page 2 into page 3. Keep ZX0's bit
        ; accumulator on the fixed page-3 stack while copying literals.
        ; Do not borrow AF': Konami engines may retain alternate-register
        ; state across initialization and playback.
        push    af
kcpz_zx0_literal_loop:
        ld      a,h
        cp      0xC0
        call    z,kcpz_advance_source_page2
        ld      a,(hl)
        inc     hl
        ld      (de),a
        inc     de
        dec     bc
        ld      a,b
        or      c
        jr      nz,kcpz_zx0_literal_loop
        pop     af
        add     a,a
        jr      c,kcpz_zx0_new_offset
        call    kcpz_zx0_elias
kcpz_zx0_copy:
        ex      (sp),hl
        push    hl
        add     hl,de
        ldir
        pop     hl
        ex      (sp),hl
        add     a,a
        jr      nc,kcpz_zx0_literals
kcpz_zx0_new_offset:
        pop     bc
        ld      c,0xFE
        call    kcpz_zx0_elias_loop
        inc     c
        ret     z
        ld      b,c
        push    af
        ld      a,h
        cp      0xC0
        call    z,kcpz_advance_source_page2
        pop     af
        ld      c,(hl)
        inc     hl
        rr      b
        rr      c
        push    bc
        ld      bc,1
        call    nc,kcpz_zx0_elias_backtrack
        inc     bc
        jr      kcpz_zx0_copy
kcpz_zx0_elias:
        inc     c
kcpz_zx0_elias_loop:
        add     a,a
        jr      nz,kcpz_zx0_elias_skip
        push    af
        ld      a,h
        cp      0xC0
        call    z,kcpz_advance_source_page2
        pop     af
        ld      a,(hl)
        inc     hl
        rla
kcpz_zx0_elias_skip:
        ret     c
kcpz_zx0_elias_backtrack:
        add     a,a
        rl      c
        rl      b
        jr      kcpz_zx0_elias_loop
; KCPZ_DECODER_END

; Compact resource-KSP / MoonSound path. The DOS2 loader has validated the
; KSP1 trailer; this path locates ENGN[0] and SONG[requested-id].
ksp_runtime_path:
        call    ksp_parse_directory
        jp      c,format_error
        call    ksp_materialize
        jp      c,format_error
        call    ksp_install_engine_patches
        call    ksp_install_page2_runtime
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P1_DISPATCH
        call    kcpz_select_ram_page2
        ld      a,(RUNTIME_KSS_TABLE+1)
        call    PUT_P2_DISPATCH
        jp      0xB800

ksp_finish_exit:
        di
        call    stop_runtime
        ld      sp,(RUNTIME_STACK_TOP)
        ei
        ld      bc,0
        jp      0x0005

ksp_parse_directory:
        ld      a,(file_size+2)
        or      a
        jp      nz,ksp_parse_bad
        ld      hl,(file_size)
        ld      de,16
        or      a
        sbc     hl,de
        jp      c,ksp_parse_bad
        call    kcpx_set_source
        call    kcpx_read_word
        ld      (ksp_directory),hl
        call    kcpx_read_word
        ld      a,h
        or      l
        jp      nz,ksp_parse_bad
        ld      hl,(ksp_directory)
        call    kcpx_set_source
        call    kcpx_read_byte
        cp      'K'
        jp      nz,ksp_parse_bad
        call    kcpx_read_byte
        cp      'D'
        jp      nz,ksp_parse_bad
        call    kcpx_read_byte
        cp      'I'
        jp      nz,ksp_parse_bad
        call    kcpx_read_byte
        cp      'R'
        jp      nz,ksp_parse_bad
        call    kcpx_read_word
        ld      de,16
        or      a
        sbc     hl,de
        jp      nz,ksp_parse_bad
        call    kcpx_read_word
        ld      de,32
        or      a
        sbc     hl,de
        jp      nz,ksp_parse_bad
        call    kcpx_read_word
        ld      a,h
        or      a
        jp      nz,ksp_parse_bad
        ld      a,l
        or      a
        jp      z,ksp_parse_bad
        ld      (ksp_entry_count),a
        call    kcpx_read_word
        xor     a
        ld      (ksp_found),a
        ld      hl,(ksp_directory)
        ld      de,16
        add     hl,de
        ld      (ksp_entry_cursor),hl
ksp_parse_entry_loop:
        call    ksp_read_entry
        jp      c,ksp_parse_bad
        call    ksp_match_entry
        ld      hl,(ksp_entry_cursor)
        ld      de,32
        add     hl,de
        ld      (ksp_entry_cursor),hl
        ld      a,(ksp_entry_count)
        dec     a
        ld      (ksp_entry_count),a
        jr      nz,ksp_parse_entry_loop
        ld      a,(ksp_found)
        and     3
        cp      3
        jr      nz,ksp_parse_bad
        or      a
        ret
ksp_parse_bad:
        scf
        ret

ksp_read_entry:
        ld      hl,(ksp_entry_cursor)
        call    kcpx_set_source
        call    kcpx_read_byte
        ld      (ksp_type+0),a
        call    kcpx_read_byte
        ld      (ksp_type+1),a
        call    kcpx_read_byte
        ld      (ksp_type+2),a
        call    kcpx_read_byte
        ld      (ksp_type+3),a
        call    ksp_read_dword16
        ret     c
        ld      (ksp_entry_id),hl
        call    ksp_read_dword16
        ret     c
        ld      (ksp_entry_offset),hl
        call    ksp_read_dword16
        ret     c
        ld      (ksp_entry_packed),hl
        call    ksp_read_dword16
        ret     c
        ld      (ksp_entry_unpacked),hl
        call    kcpx_read_word         ; CRC32, validated by host tools
        call    kcpx_read_word
        call    kcpx_read_word
        ld      a,h
        or      a
        jr      nz,ksp_parse_bad
        ld      a,l
        cp      2
        jr      nc,ksp_parse_bad
        ld      (ksp_entry_compression),a
        call    kcpx_read_word
        or      a
        ret

ksp_read_dword16:
        call    kcpx_read_word
        push    hl
        call    kcpx_read_word
        ld      a,h
        or      l
        pop     hl
        ret     z
        scf
        ret

ksp_match_entry:
        ld      hl,ksp_type
        ld      a,(hl)
        cp      'E'
        jr      nz,ksp_match_song
        inc     hl
        ld      a,(hl)
        cp      'N'
        ret     nz
        inc     hl
        ld      a,(hl)
        cp      'G'
        ret     nz
        inc     hl
        ld      a,(hl)
        cp      'N'
        ret     nz
        ld      hl,(ksp_entry_id)
        ld      a,h
        or      l
        ret     nz
        ld      hl,(ksp_entry_offset)
        ld      (ksp_engine_offset),hl
        ld      hl,(ksp_entry_packed)
        ld      (ksp_engine_packed),hl
        ld      hl,(ksp_entry_unpacked)
        ld      (ksp_engine_size),hl
        ld      a,(ksp_entry_compression)
        ld      (ksp_engine_compression),a
        ld      a,(ksp_found)
        or      1
        ld      (ksp_found),a
        ret
ksp_match_song:
        ld      hl,ksp_type
        ld      a,(hl)
        cp      'S'
        ret     nz
        inc     hl
        ld      a,(hl)
        cp      'O'
        ret     nz
        inc     hl
        ld      a,(hl)
        cp      'N'
        ret     nz
        inc     hl
        ld      a,(hl)
        cp      'G'
        ret     nz
        ld      hl,(ksp_entry_id)
        ld      a,h
        or      a
        ret     nz
        ld      a,(song_number)
        cp      l
        ret     nz
        ld      hl,(ksp_entry_offset)
        ld      (ksp_song_offset),hl
        ld      hl,(ksp_entry_packed)
        ld      (ksp_song_packed),hl
        ld      hl,(ksp_entry_unpacked)
        ld      (ksp_song_size),hl
        ld      a,(ksp_entry_compression)
        ld      (ksp_song_compression),a
        ld      a,(ksp_found)
        or      2
        ld      (ksp_found),a
        ret

ksp_materialize:
        ld      hl,(load_address)
        ld      de,0x4000
        or      a
        sbc     hl,de
        jp      nz,ksp_parse_bad
        ld      hl,(ksp_engine_size)
        ld      a,h
        cp      0x40
        jp      nc,ksp_parse_bad
        ld      hl,(ksp_song_size)
        ld      a,h
        cp      0x37                ; reserve B700H-BFFFH for stack/runtime
        jp      nc,ksp_parse_bad
        ld      a,(ksp_engine_compression)
        or      a
        jr      z,ksp_engine_stream_ok
        ld      hl,(ksp_engine_offset)
        ld      de,(ksp_engine_packed)
        call    ksp_check_single_stage
        jp      c,ksp_parse_bad
ksp_engine_stream_ok:
        ld      a,(ksp_song_compression)
        or      a
        jr      z,ksp_song_stream_ok
        ld      hl,(ksp_song_offset)
        ld      de,(ksp_song_packed)
        call    ksp_check_single_stage
        jp      c,ksp_parse_bad
ksp_song_stream_ok:

        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P1_DISPATCH
        xor     a
        ld      hl,0x4000
        ld      de,0x4001
        ld      bc,0x3FFF
        ld      (hl),a
        ldir
        call    kcpz_select_ram_page2
        ld      hl,(ksp_engine_offset)
        call    kcpx_set_source
        call    kcpz_map_source_page2
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x80
        ld      h,a
        ld      de,0x4000
        ld      a,(ksp_engine_compression)
        or      a
        jr      nz,ksp_decode_engine
        ld      bc,(ksp_engine_size)
        ldir
        jr      ksp_engine_done
ksp_decode_engine:
        call    kcpz_zx0_decoder
ksp_engine_done:
        ld      hl,(ksp_song_offset)
        call    kcpx_set_source
        call    map_source_page1
        ld      a,(RUNTIME_KSS_TABLE+1)
        call    PUT_P2_DISPATCH
        xor     a
        ld      hl,0x8000
        ld      de,0x8001
        ld      bc,0x3FFF
        ld      (hl),a
        ldir
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,0x8000
        ld      a,(ksp_song_compression)
        or      a
        jr      nz,ksp_decode_song
        call    ksp_copy_raw_song
        jr      ksp_song_done
ksp_decode_song:
        call    kcpz_zx0_decoder
ksp_song_done:
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P1_DISPATCH
        call    ksp_compact_mwm
        ret     c
        ; Third allocated segment becomes page-3 MBWave work RAM only while
        ; the page-2 resident playback loop is active.
        ld      a,(RUNTIME_KSS_TABLE+2)
        call    PUT_P2_DISPATCH
        xor     a
        ld      hl,0x8000
        ld      de,0x8001
        ld      bc,0x3FFF
        ld      (hl),a
        ldir
        ld      a,(RUNTIME_KSS_TABLE+1)
        call    PUT_P2_DISPATCH
        or      a
        ret

ksp_check_single_stage:
        ld      a,h
        and     0x3F
        ld      h,a
        add     hl,de
        jr      c,ksp_stage_bad
        ld      a,h
        cp      0x41
        jr      nc,ksp_stage_bad
        or      a
        ret
ksp_stage_bad:
        scf
        ret

; Copy an uncompressed SONG across staged-file segment boundaries while the
; destination mapper segment remains visible in page 2.
ksp_copy_raw_song:
        ld      hl,0x8000
        ld      (kcp_dest_offset),hl
        ld      hl,(ksp_song_size)
        ld      (kcp_remaining),hl
ksp_copy_raw_song_loop:
        ld      hl,(kcp_remaining)
        ld      a,h
        or      l
        ret     z
        ld      bc,(kcp_remaining)
        call    source_boundary
        call    cap_bc_hl
        ld      (chunk_size),bc
        call    map_source_page1
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,(kcp_dest_offset)
        ld      bc,(chunk_size)
        ldir
        call    advance_source
        ld      hl,(kcp_dest_offset)
        ld      de,(chunk_size)
        add     hl,de
        ld      (kcp_dest_offset),hl
        ld      hl,(kcp_remaining)
        ld      de,(chunk_size)
        or      a
        sbc     hl,de
        ld      (kcp_remaining),hl
        jr      ksp_copy_raw_song_loop

ksp_compact_mwm:
        ld      a,(0x8000)
        cp      'M'
        jp      nz,ksp_parse_bad
        ld      hl,0x811C
        ld      a,(0x8006)
        inc     a
        ld      b,a
        ld      c,0
ksp_mwm_position_loop:
        ld      a,(hl)
        cp      c
        jr      c,ksp_mwm_position_next
        ld      c,a
ksp_mwm_position_next:
        inc     hl
        djnz    ksp_mwm_position_loop
        ld      a,c
        inc     a
        ld      (mwm_patterns_left),a
        add     a,a
        ld      e,a
        ld      d,0
        add     hl,de
        ld      (mwm_raw_cursor),hl
        ld      (mwm_compact_cursor),hl
ksp_mwm_block_loop:
        ld      hl,(mwm_raw_cursor)
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        inc     hl
        ld      a,(hl)
        inc     hl
        or      a
        jp      z,ksp_parse_bad
        ld      e,a
        ld      a,(mwm_patterns_left)
        sub     e
        jp      c,ksp_parse_bad
        ld      (mwm_patterns_left),a
        ld      de,(mwm_compact_cursor)
        ldir
        ld      (mwm_raw_cursor),hl
        ld      (mwm_compact_cursor),de
        ld      a,(mwm_patterns_left)
        or      a
        jr      nz,ksp_mwm_block_loop
        ld      hl,0x8000
        ld      bc,(ksp_song_size)
        add     hl,bc
        ld      de,(mwm_raw_cursor)
        or      a
        sbc     hl,de
        ld      b,h
        ld      c,l
        ld      hl,(mwm_raw_cursor)
        ld      de,(mwm_compact_cursor)
        ldir
        or      a
        ret

ksp_install_engine_patches:
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P1_DISPATCH
        ld      a,0xC9
        ld      (0x7CA3),a
        ld      hl,ksp_rslreg_stub
        ld      de,0x7CA8
        ld      bc,ksp_rslreg_stub_end-ksp_rslreg_stub
        ldir
        xor     a
        ld      (0x4535),a          ; keep interrupts off while page 3 is work RAM
        ld      (0x7E88),a
        ld      (0x7E89),a
        ld      hl,ksp_poll_return_stub
        ld      de,0x46AC
        ld      bc,ksp_poll_return_stub_end-ksp_poll_return_stub
        ldir
        ld      hl,ksp_mbwave_bootstrap
        ld      de,(init_address)
        ld      bc,ksp_mbwave_bootstrap_end-ksp_mbwave_bootstrap
        ldir
        ret
ksp_rslreg_stub:
        xor     a
        ret
ksp_rslreg_stub_end:
ksp_poll_return_stub:
        ret
        nop
        nop
        nop
        nop
ksp_poll_return_stub_end:
ksp_mbwave_bootstrap:
        xor     a
        ld      hl,0xDA00
        ld      b,0x24
ksp_mbwave_clear:
        ld      (hl),a
        inc     hl
        djnz    ksp_mbwave_clear
        ld      hl,0x8006
        ld      (0xDA04),hl
        ld      a,0x03
        ld      (0xDA01),a
        ld      (0xDA02),a
        ld      (0xDA03),a
        call    0x4042
        ret
ksp_mbwave_bootstrap_end:

ksp_install_page2_runtime:
        ld      hl,ksp_page2_runtime
        ld      de,0xB800
        ld      bc,ksp_page2_runtime_end-ksp_page2_runtime
        ldir
        ld      a,(RUNTIME_KSS_TABLE+2)
        ld      (0xB800+(ksp_page2_work_segment-ksp_page2_runtime)+1),a
        ld      a,(RUNTIME_MAIN_TABLE+3)
        ld      (0xB800+(ksp_page2_original_p3-ksp_page2_runtime)+1),a
        ld      hl,(init_address)
        ld      (0xB800+(ksp_page2_init_target-ksp_page2_runtime)+1),hl
        ld      hl,(play_address)
        ld      (0xB800+(ksp_page2_play_target-ksp_page2_runtime)+1),hl
        ret

; This block is copied to B800H. All internal control transfers are relative
; or explicitly based on B800H, so it remains valid after page 3 is replaced.
ksp_page2_runtime:
        di
        ld      sp,0xB700
ksp_page2_work_segment:
        ld      a,0
        out     (0xFF),a
        ld      hl,0xB800+(ksp_page2_after_init-ksp_page2_runtime)
        push    hl
        xor     a
ksp_page2_init_target:
        ld      hl,0
        jp      (hl)
ksp_page2_after_init:
        di
        ld      a,0xC9
        ld      (0x46AC),a
        xor     a
        ld      (0x46AD),a
        ld      (0x46AE),a
        ld      (0x46AF),a
        ld      (0x46B0),a
        ; Select VDP status register 0. Polling its vblank flag provides a
        ; CPU-speed-independent 60 Hz clock on Z80 and R800 machines.
        out     (0x99),a
        ld      a,0x8F
        out     (0x99),a
ksp_page2_play_loop:
        in      a,(0x99)
        and     0x80
        jr      z,ksp_page2_play_loop
        ld      hl,0xB800+(ksp_page2_after_play-ksp_page2_runtime)
        push    hl
ksp_page2_play_target:
        ld      hl,0
        jp      (hl)
ksp_page2_after_play:
        in      a,(0xAA)
        ld      e,a
        and     0xF0
        or      7
        out     (0xAA),a
        in      a,(0xA9)
        cpl
        bit     2,a
        ld      a,e
        out     (0xAA),a
        jr      z,ksp_page2_play_loop

        di
        ld      a,4
        out     (0xC0),a
        ld      a,0x80
        out     (0xC1),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        ; Globally attenuate both OPL3/FM and OPL4/PCM outputs. This is
        ; independent of envelope release times and guarantees immediate
        ; silence even if a song left unusual per-channel state behind.
        ld      a,0xF8
        out     (0xC4),a
        ld      a,0x3F
        out     (0xC5),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        ld      a,0xF9
        out     (0xC4),a
        ld      a,0x3F
        out     (0xC5),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        ld      d,0xB0
        ld      b,9
ksp_page2_silence_fm0:
        ld      a,d
        out     (0xC0),a
        xor     a
        out     (0xC1),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        inc     d
        djnz    ksp_page2_silence_fm0
        ld      d,0xB0
        ld      b,9
ksp_page2_silence_fm1:
        ld      a,d
        out     (0xC2),a
        xor     a
        out     (0xC3),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        inc     d
        djnz    ksp_page2_silence_fm1
        ; Maximum attenuation mutes every wave slot immediately. Plain
        ; key-off alone follows the sample's release rate and sounds stuck.
        ld      d,0x50
        ld      b,24
ksp_page2_silence_wave_level:
        ld      a,d
        out     (0xC4),a
        ld      a,0xFF
        out     (0xC5),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        inc     d
        djnz    ksp_page2_silence_wave_level
        ld      d,0x68
        ld      b,24
ksp_page2_silence_wave:
        ld      a,d
        out     (0xC4),a
        ld      a,0x40              ; key off + damp
        out     (0xC5),a
        call    0xB800+(ksp_page2_opl_wait-ksp_page2_runtime)
        inc     d
        djnz    ksp_page2_silence_wave
        jr      ksp_page2_silence_done
ksp_page2_opl_wait:
        push    bc
        ld      b,16
ksp_page2_opl_wait_loop:
        in      a,(0xC0)
        djnz    ksp_page2_opl_wait_loop
        pop     bc
        ret
ksp_page2_silence_done:
ksp_page2_original_p3:
        ld      a,0
        out     (0xFF),a
        jp      ksp_finish_exit
ksp_page2_runtime_end:

; Generic complete-page images have a one-byte load image at file offset 20H
; and their KCPX/KCPZ descriptor immediately after it at 21H.
probe_kcpx:
        xor     a
        ld      (kcp_format),a
        ld      a,(header+2)
        cp      'S'
        ret     nz
        ld      a,(RUNTIME_STAGE_TABLE)
        call    PUT_P1_DISPATCH
        ld      a,(0x4021)
        cp      'K'
        jr      nz,probe_kcpx_restore
        ld      b,5
probe_kcpx_prefix_ok:
        ld      a,(0x4022)
        cp      'C'
        jr      nz,probe_kcpx_restore
        ld      a,(0x4023)
        cp      'P'
        jr      nz,probe_kcpx_restore
        ld      a,(0x4024)
        cp      'X'
        jr      z,probe_kcpx_found
        cp      'Z'
        jr      nz,probe_kcpx_restore
        ld      a,b
        inc     a
        ld      (kcp_format),a
        jr      probe_kcpx_restore
probe_kcpx_found:
        ld      a,b
        ld      (kcp_format),a
probe_kcpx_restore:
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ret

player_end:
