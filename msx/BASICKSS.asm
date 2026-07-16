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
; KSS bank data is materialized in dynamically allocated segments.  The
; runtime logical-to-physical table translates engine bank requests before
; page 2 is switched, so the engine never depends on these segment numbers.
; The bootstrap is assembled at C000H.  For the DOS2 Quarth path the build
; assembles the same source a second time at 52E4H; after materialization the
; bootstrap copies that page-1 runtime over the free tail of the Quarth image.

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
        ; KSSPLAY.COM always supplies the DOS2 mapper-call handoff. Keep the
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
        ; QCPX is the complete-page Quarth format.  Its materializer uses
        ; only page 1 as a temporary source/destination window; page 0 and
        ; page 3 remain in their DOS2 mappings and page 2 is reserved for
        ; the real SCC slot.  The legacy path retains the older RAM layout.
        ld      a,(qcp_format)
        or      a
        jp      nz,qcpx_runtime_path

        call    map_ram_pages
        ; The bootstrap is temporary.  Quarth's page-3 channel work areas
        ; must remain untouched once the page-1 runtime takes over.

player_relocated:
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

        ; Quarth's initial image ends exactly where the page-1 runtime is
        ; installed.  Other engines retain the older page-3 runtime path.
        jp      is_quarth_dispatch

legacy_runtime_path:
        call    install_page0_stubs
        call    install_runtime_stubs

        ; Install the resident runtime trampoline and dynamic state while
        ; page 3 still contains the fixed TPA player.
        call    install_init_trampoline

        ; Page 1 is the KSS RAM view.  Page 2 is either the 8K working RAM
        ; view or the first 16K bank. Page 3 remains the fixed resident TPA.
        ld      a,(RUNTIME_MAIN_TABLE+1)
        call    PUT_P1_DISPATCH

        ld      a,(bank_mode)
        and     0x80
        jr      nz,init_page2_is_ram
        ld      a,(bank_count)
        or      a
        jr      z,init_page2_is_ram
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P2_DISPATCH
        jr      init_page2_ready
init_page2_is_ram:
        ld      a,(RUNTIME_MAIN_TABLE+2)
        call    PUT_P2_DISPATCH
init_page2_ready:
        jp      init_trampoline

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
        ; Page 0 is still the BIOS at every error path.
        call    print_string
error_halt:
        jr      error_halt

; Read the first 32 bytes from staged mapper bank 16 into the relocated
; player's private header buffer.
read_header:
        ; Read through page 1.  This is important for QCPX: page 2 must
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
        call    probe_qcpx
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
; segment 0 deliberately remains untouched: the DOS2 COM loader stores the
; page-1 runtime blob in that physical segment until Quarth has been fully
; materialized.  The fixed page-3 TPA segment is also excluded.
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
        ; RST 28H/RST 30H vectors.  Keep the RST 28H route mode-dependent:
        ; Quarth's 16K image uses RST 28H, while the known 8K adapter keeps
        ; RST 28H for page-3/8K data and RST 30H for page-2 data.
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
fast_8k:        defb    0
ram_slot:       defb    0
song_number:    defb    0
; The DOS2 loader fills this block after copying the player to C000H.  It is
; copied along with the player when start_player relocates the code.
        defs    RUNTIME_CONFIG-$,0
runtime_config:
        defs    RUNTIME_CONFIG_END-RUNTIME_CONFIG,0

; QCPX materializer state.  This follows the fixed handoff block so the
; original bootstrap still reaches RUNTIME_CONFIG at exactly C960H.
qcp_format:     defb    0
qcp_page_count: defb    0
qcp_track_count:defb    0
qcp_page_index: defb    0
qcp_selected_page: defb 0
qcp_materialized_page: defb 0xFF
qcp_original_song:  defb 0
qcp_current_song:   defb 0
qcp_key_latch:      defb 0
qcp_dest_segment:   defb 0
qcp_selected_segment:defb 0
qcp_value:      defb    0
qcp_scc_primary_bits:defb 0
qcp_engine_size:    defw 0
qcp_common_size:    defw 0
qcp_page_data_address:defw 0
qcp_engine_source:  defw 0
qcp_common_source:  defw 0
qcp_records_source: defw 0
qcp_data_source:defw 0
qcp_patch_source:defw 0
qcp_engine_compressed_size:defw 0
qcp_common_compressed_size:defw 0
qcp_data_compressed_size:defw 0
qcp_cursor:     defw    0
qcp_data_size:  defw    0
qcp_patch_count:defw    0
qcp_patch_left: defw    0
qcp_patch_offset:defw  0
qcp_patch_value:defw    0
qcp_dest_offset:defw    0
qcp_remaining:  defw    0
qcp_chunk_size: defw    0

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

; Detect the repacked raw Quarth image. The page-1 runtime layout is only
; safe for this image because its main image ends at 52E4H.
is_quarth_dispatch:
        ld      hl,(load_address)
        ld      de,0x4000
        or      a
        sbc     hl,de
        jp      nz,legacy_runtime_path
        ; copy_initial_image consumes remaining and leaves destination at the
        ; end of the main image, so test that final address here.
        ld      hl,(destination)
        ld      de,0x52E4
        or      a
        sbc     hl,de
        jp      nz,legacy_runtime_path
        ld      hl,(init_address)
        ld      de,0x52BB
        or      a
        sbc     hl,de
        jp      nz,legacy_runtime_path
        ld      hl,(play_address)
        ld      de,0x52DF
        or      a
        sbc     hl,de
        jp      nz,legacy_runtime_path
        ld      a,(bank_mode)
        cp      0x02
        jp      nz,legacy_runtime_path
        ; The exact Quarth signature above already identifies the repacked
        ; two-bank image.  Do not make the hand-off depend on the mutable
        ; parser bookkeeping byte after the bank materialization loops.
        jp      prepare_quarth_runtime

; Move the page-1 runtime over the free tail of Quarth's main image. The
; bootstrap remains in page 3 only until this copy has completed; no player
; code or player buffers remain active there during playback.
prepare_quarth_runtime:
        ld      a,(RUNTIME_MAIN_TABLE+1)
        call    PUT_P1_DISPATCH
        ld      hl,(RUNTIME_BLOB_SOURCE)
        ld      de,PLAYER_RUNTIME_BASE
        ld      bc,(RUNTIME_BLOB_SIZE)
        ldir
        ld      hl,RUNTIME_CONFIG
        ld      de,PLAYER_RUNTIME_CONFIG_TARGET
        ld      bc,RUNTIME_CONFIG_END-RUNTIME_CONFIG
        ldir
        ld      a,(RUNTIME_SCC_SLOT)
        ld      (PLAYER_RUNTIME_SCC_SLOT_TARGET),a
        ld      a,(RUNTIME_SCC_SLOT_VALID)
        ld      (PLAYER_RUNTIME_SCC_SLOT_VALID_TARGET),a
        jp      PLAYER_RUNTIME_ENTRY

; Entry target for the page-1 build. It is reached through the three-byte
; trampoline inserted before copy_field by the build script. The image and
; both KSS banks are already materialized, so no page-1 source window is ever
; needed after this point.
runtime_ready_impl:
        di
        ld      sp,(RUNTIME_STACK_TOP)
        call    install_dispatches
        ld      hl,(BASIC_SIZE)
        ld      (file_size),hl
        ld      a,(BASIC_SIZE+2)
        ld      (file_size+2),a
        ld      a,(BASIC_SONG)
        ld      (song_number),a
        call    read_header
        jp      c,format_error
        call    install_page0_stubs
        call    install_runtime_stubs
        call    install_init_trampoline
        ld      a,(RUNTIME_MAIN_TABLE+1)
        call    PUT_P1_DISPATCH
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P2_DISPATCH
        jp      init_trampoline

; -------------------------------------------------------------------------
; QCPX complete-page Quarth path
;
; QCPX stores one engine template and one common-data template, followed by
; five page records.  The DOS2 player expands each record into an allocated
; mapper segment.  All copying is done through page 1, using a reserved
; page-3 TPA block as a 0400H scratch window because a Z80 cannot expose both a
; staging segment and a destination segment in the same mapper page at once.
; Page 2 is never used as a RAM copy window here: after materialization it is
; selected to the SCC cartridge and stays there for INIT and PLAY.

qcpx_runtime_path:
        call    qcpx_parse
        jp      c,format_error
        ld      a,0xFF
        ld      (qcp_materialized_page),a
        ld      a,(song_number)
        ld      (qcp_current_song),a
        call    qcpx_get_song_mapping
        jp      c,format_error
        call    qcpx_materialize_selected
        jp      c,format_error

        ; Keep the QCPX parser/materializer resident for live track changes.
        ; The legacy runtime installer would overwrite it with bank handlers
        ; and SCC shadow buffers, none of which this complete-page path uses.
        ld      hl,qcpx_return_stub
        ld      de,RETURN_STUB
        ld      bc,qcpx_return_stub_end-qcpx_return_stub
        ldir
        xor     a
        ld      (HTIMI_INSTALLED),a

        ; Install a direct QCPX wrapper outside the retained materializer.
        ; BIOS/DOS can
        ; restore its normal mapper and slot layout while servicing HALT, so
        ; every PLAY must first reassert page 1 and the page-2 SCC slot.
        ;
        ;   LD A,page1_segment / CALL PUT_P1
        ;   LD A,scc_slot / LD H,80H / CALL CUSTOM_ENASLT
        ;   LD HL,play / LD DE,return / PUSH DE / JP (HL) / RET
        ld      hl,PLAY_WRAPPER
        ld      (hl),0x3E
        inc     hl
        ld      a,(qcp_selected_segment)
        ld      (hl),a
        inc     hl
        ld      (hl),0xCD
        inc     hl
        ld      (hl),0x10
        inc     hl
        ld      (hl),0xD0
        inc     hl
        ld      (hl),0x3E
        inc     hl
        ld      a,(RUNTIME_SCC_SLOT)
        ld      (hl),a
        inc     hl
        ld      (hl),0x26
        inc     hl
        ld      (hl),0x80
        inc     hl
        ld      (hl),0xCD
        inc     hl
        ld      (hl),CUSTOM_ENASLT & 0xFF
        inc     hl
        ld      (hl),CUSTOM_ENASLT >> 8
        inc     hl
        ld      (hl),0x21
        inc     hl
        ld      de,(play_address)
        ld      (hl),e
        inc     hl
        ld      (hl),d
        inc     hl
        ld      (hl),0x11
        inc     hl
        ld      de,PLAY_WRAPPER+20
        ld      (hl),e
        inc     hl
        ld      (hl),d
        inc     hl
        ld      (hl),0xD5
        inc     hl
        ld      (hl),0xE9
        inc     hl
        ld      (hl),0xC9

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
        ld      (qcp_scc_primary_bits),a
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
        ld      a,(qcp_scc_primary_bits)
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
        ; QCPX keeps page 2 on this slot for the complete playback lifetime.
        ld      a,0x3F
        ld      (0x9000),a

        ld      a,(qcp_original_song)
        ld      (song_number),a
        call    install_init_trampoline
        ld      a,(qcp_selected_segment)
        call    PUT_P1_DISPATCH
        jp      init_trampoline

; The copied loop remains outside page 1, so it can replace the complete
; engine/music page while changing tracks.
qcpx_return_stub:
        im      1
        ei
qcpx_return_halt:
        halt
        call    PLAY_WRAPPER
        di
        ; Quarth happens to retain useful Z80 state between PLAY calls.
        ; Keyboard scanning is player bookkeeping and must be invisible to
        ; the engine when no track change is requested.
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
        call    qcpx_poll_keyboard
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ei
        jr      qcpx_return_halt
qcpx_return_stub_end:

qcpx_poll_keyboard:
        ; Poll keyboard row 8 directly through the PPI. BIOS keyboard calls
        ; can invoke slot-dependent code while page 1 contains QCPX RAM.
        ; Row 8: bit 0=Right, bit 3=Left, bit 7=Space (active low).
        in      a,(0xAA)
        ld      c,a
        and     0xF0
        or      8
        out     (0xAA),a
        in      a,(0xA9)
        cpl
        and     0x89
        ld      b,a
        ld      a,c
        out     (0xAA),a
        ld      a,b
        or      a
        jr      nz,qcpx_key_pressed
        xor     a
        ld      (qcp_key_latch),a
        ret
qcpx_key_pressed:
        ld      c,a
        ld      a,(qcp_key_latch)
        or      a
        ret     nz
        ld      a,1
        ld      (qcp_key_latch),a
        bit     0,c
        jr      nz,qcpx_next_song
        bit     3,c
        jr      nz,qcpx_previous_song
        bit     7,c
        ret     z
        ld      a,(qcp_current_song)
        jp      qcpx_switch_song

qcpx_next_song:
        ld      a,(qcp_current_song)
        inc     a
        ld      c,a
        ld      a,(qcp_track_count)
        cp      c
        ld      a,c
        jr      nz,qcpx_switch_song
        xor     a
        jr      qcpx_switch_song

qcpx_previous_song:
        ld      a,(qcp_current_song)
        or      a
        jr      nz,qcpx_previous_decrement
        ld      a,(qcp_track_count)
qcpx_previous_decrement:
        dec     a

qcpx_switch_song:
        di
        ld      (qcp_current_song),a
        ld      (song_number),a
        call    qcpx_silence
        call    qcpx_get_song_mapping
        jp      c,format_error
        call    qcpx_materialize_selected
        jp      c,format_error
        ld      a,(qcp_original_song)
        ld      (song_number),a
        call    install_init_trampoline
        ld      a,(qcp_selected_segment)
        call    PUT_P1_DISPATCH
        ld      a,(RUNTIME_SCC_SLOT)
        ld      h,0x80
        call    CUSTOM_ENASLT
        ld      a,0x3F
        ld      (0x9000),a
        jp      init_trampoline

qcpx_silence:
        xor     a
        ld      hl,0x988A
        ld      b,6
qcpx_silence_scc:
        ld      (hl),a
        inc     hl
        djnz    qcpx_silence_scc
        ld      b,3
        ld      c,8
qcpx_silence_psg:
        ld      a,c
        out     (0xA0),a
        xor     a
        out     (0xA1),a
        inc     c
        djnz    qcpx_silence_psg
        ret

qcpx_parse:
        ; Header fields are little-endian and live at QCPX+04..0F.
        ld      hl,0x25
        call    qcpx_set_source
        call    qcpx_read_byte
        cp      1
        jp      nz,qcpx_parse_bad
        call    qcpx_read_byte
        or      a
        jp      z,qcpx_parse_bad
        cp      33
        jp      nc,qcpx_parse_bad
        ld      (qcp_page_count),a
        call    qcpx_read_byte
        ld      (qcp_track_count),a
        call    qcpx_read_byte

        ld      hl,0x29
        call    qcpx_set_source
        call    qcpx_read_word
        ld      (qcp_engine_size),hl
        call    qcpx_read_word
        ld      (qcp_common_size),hl
        call    qcpx_read_word
        ld      (qcp_page_data_address),hl
        call    qcpx_read_word

        ; The current Quarth builder deliberately fixes these addresses.  A
        ; mismatch is rejected instead of silently materializing a corrupt
        ; page with incorrect absolute references.
        ld      hl,(qcp_page_data_address)
        ld      de,0x65B5
        or      a
        sbc     hl,de
        jr      nz,qcpx_parse_bad
        ld      hl,(qcp_engine_size)
        ld      a,h
        cp      0x40
        jr      nc,qcpx_parse_bad
        ld      hl,(qcp_common_size)
        ld      a,h
        cp      0x40
        jr      nc,qcpx_parse_bad

        ld      a,(qcp_format)
        cp      2
        jr      z,qcpz_parse_sources

        ; The shared blobs and page records are addressed from the original
        ; file, whose QCPX signature starts at offset 21H.
        ld      hl,0x231
        ld      (qcp_engine_source),hl
        ld      de,(qcp_engine_size)
        add     hl,de
        ld      (qcp_common_source),hl
        ld      de,(qcp_common_size)
        add     hl,de
        ld      (qcp_records_source),hl
        or      a
        ret

; QCPZ_PARSE_BEGIN
qcpz_parse_sources:
        ; QCPZ's fixed directory follows the two 256-byte song maps. Stream
        ; offsets are relative to the QCPZ magic at file offset 21H.
        ld      hl,0x231
        call    qcpx_set_source
        call    qcpx_read_word
        ld      (qcp_engine_compressed_size),hl
        call    qcpx_read_word
        ld      de,0x21
        add     hl,de
        ld      (qcp_engine_source),hl
        call    qcpx_read_word
        ld      (qcp_common_compressed_size),hl
        call    qcpx_read_word
        ld      de,0x21
        add     hl,de
        ld      (qcp_common_source),hl
        ld      hl,0x239
        ld      (qcp_records_source),hl
        or      a
        ret
; QCPZ_PARSE_END
qcpx_parse_bad:
        scf
        ret

; Map the source-positioned byte and return it in A.  The original page-1
; segment is restored before returning so every metadata read is isolated.
; The source position is advanced by one byte.
qcpx_read_byte:
        call    map_source_page1
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      a,(hl)
        ld      (qcp_value),a
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ld      hl,(source_position)
        inc     hl
        ld      (source_position),hl
        ld      a,h
        or      l
        jr      nz,qcpx_read_byte_no_carry
        ld      a,(source_position+2)
        inc     a
        ld      (source_position+2),a
qcpx_read_byte_no_carry:
        ld      a,(qcp_value)
        ret

qcpx_read_word:
        call    qcpx_read_byte
        push    af
        call    qcpx_read_byte
        ld      h,a
        pop     af
        ld      l,a
        ret

qcpx_set_source:
        ld      (source_position),hl
        xor     a
        ld      (source_position+2),a
        ret

; Translate the new contiguous song ID to the original Quarth selector and
; to its complete mapper page.
qcpx_get_song_mapping:
        ld      a,(song_number)
        ld      c,a
        ld      b,0
        ld      hl,0x31              ; QCPX + 10H
        add     hl,bc
        call    qcpx_set_source
        call    qcpx_read_byte
        cp      0xFF
        jr      z,qcpx_mapping_bad
        ld      (qcp_original_song),a

        ; qcpx_read_byte/map_source_page1 uses C internally. Reload the
        ; requested contiguous song ID before indexing the page table;
        ; otherwise every song accidentally materializes logical page 0.
        ld      a,(song_number)
        ld      c,a
        ld      b,0
        ld      hl,0x131             ; QCPX + 110H
        add     hl,bc
        call    qcpx_set_source
        call    qcpx_read_byte
        cp      0xFF
        jr      z,qcpx_mapping_bad
        ld      c,a
        ld      a,(qcp_page_count)
        cp      c
        jr      c,qcpx_mapping_bad
        jr      z,qcpx_mapping_bad
        ld      a,c
        ld      (qcp_selected_page),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (qcp_selected_segment),a
        or      a
        ret
qcpx_mapping_bad:
        scf
        ret

qcpx_materialize_selected:
        ld      a,(qcp_format)
        cp      2
        jp      z,qcpz_materialize_selected
        ; Only one physical page is required on MSX.  The selected logical
        ; record is expanded into the first allocated KSS segment; selecting
        ; another song later simply rebuilds this same segment.
        ld      a,(qcp_selected_page)
        ld      (qcp_page_index),a
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (qcp_selected_segment),a
        call    qcpx_materialize_page
        ret

; QCPZ keeps the already expanded engine/common templates in the one page-1
; destination. A same-page song change only resets runtime state. A cross-page
; change replaces the payload and reapplies that page's pointer patches.
; QCPZ_BOOTSTRAP_BEGIN
qcpz_materialize_selected:
        ld      a,(RUNTIME_KSS_TABLE)
        ld      (qcp_selected_segment),a
        ld      (qcp_dest_segment),a
        call    qcpz_read_selected_descriptor
        ld      a,(qcp_materialized_page)
        cp      0xFF
        jr      z,qcpz_materialize_initial
        ld      c,a
        ld      a,(qcp_selected_page)
        cp      c
        jr      z,qcpz_reset_workspace

        ; Keep engine/common intact. Remove stale payload and writable state,
        ; then expand only the newly selected logical music page.
        ld      a,(qcp_dest_segment)
        call    PUT_P1_DISPATCH
        xor     a
        ld      (0x65B5),a
        ld      hl,0x65B5
        ld      de,0x65B6
        ld      bc,0x1A4A
        ldir
        jr      qcpz_expand_payload

qcpz_materialize_initial:
        ld      a,(qcp_dest_segment)
        call    PUT_P1_DISPATCH
        xor     a
        ld      (0x4000),a
        ld      hl,0x4000
        ld      de,0x4001
        ld      bc,0x3FFF
        ldir

        ld      hl,(qcp_engine_source)
        call    qcpx_set_source
        ld      bc,(qcp_engine_compressed_size)
        ld      (qcp_data_compressed_size),bc
        ld      de,0x4000
        call    qcpz_decompress_source

        ld      hl,(qcp_common_source)
        call    qcpx_set_source
        ld      bc,(qcp_common_compressed_size)
        ld      (qcp_data_compressed_size),bc
        ld      de,0x4000
        ld      hl,(qcp_engine_size)
        add     hl,de
        ex      de,hl
        call    qcpz_decompress_source

qcpz_expand_payload:
        ld      hl,(qcp_data_source)
        call    qcpx_set_source
        ld      de,(qcp_page_data_address)
        call    qcpz_decompress_source
        ld      hl,(qcp_patch_source)
        call    qcpx_set_source
        call    qcpx_apply_patches
        ld      a,(qcp_selected_page)
        ld      (qcp_materialized_page),a
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        or      a
        ret

qcpz_reset_workspace:
        ld      a,(qcp_dest_segment)
        call    PUT_P1_DISPATCH
        xor     a
        ld      (0x7D00),a
        ld      hl,0x7D00
        ld      de,0x7D01
        ld      bc,0x028B
        ldir
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        or      a
        ret

qcpz_read_selected_descriptor:
        ; Five fixed ten-byte descriptors: raw size, patch count, compressed
        ; size, compressed stream offset, patch-table offset.
        ld      a,(qcp_selected_page)
        ld      e,a
        ld      d,0
        ld      l,e
        ld      h,d
        add     hl,hl
        add     hl,hl
        add     hl,de
        add     hl,hl
        ld      de,(qcp_records_source)
        add     hl,de
        call    qcpx_set_source
        call    qcpx_read_word
        ld      (qcp_data_size),hl
        call    qcpx_read_word
        ld      (qcp_patch_count),hl
        call    qcpx_read_word
        ld      (qcp_data_compressed_size),hl
        call    qcpx_read_word
        ld      de,0x21
        add     hl,de
        ld      (qcp_data_source),hl
        call    qcpx_read_word
        ld      de,0x21
        add     hl,de
        ld      (qcp_patch_source),hl
        ret

qcpz_decompress_source:
        ; Source stream is guaranteed by the builder to remain within one
        ; 16K staged-file segment. Temporarily replace SCC page 2 by mapper
        ; RAM, decode into the persistent page-1 destination, then let the
        ; caller restore SCC after all materialization has completed.
        push    de
        call    qcpz_select_ram_page2
        call    qcpz_map_source_page2
        pop     de
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x80
        ld      h,a
        call    qcpz_zx0_decoder
        ret

qcpz_select_ram_page2:
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

qcpz_map_source_page2:
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
        call    PUT_P2_DISPATCH
        di
        ret
; QCPZ_BOOTSTRAP_END

qcpx_materialize_page:
        ; Select the one physical destination segment reserved for QCPX.
        ld      a,(qcp_selected_segment)
        ld      (qcp_dest_segment),a

        ; Match the host QCPX materializer: a newly allocated mapper segment
        ; is not guaranteed to contain zeroes, while Quarth assumes its
        ; relocated work area starts clear. Clear the whole complete page
        ; before installing engine, common data and the selected payload.
        call    PUT_P1_DISPATCH
        xor     a
        ld      (0x4000),a
        ld      hl,0x4000
        ld      de,0x4001
        ld      bc,0x3FFF
        ldir
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH

        ; Engine template at complete-page offset 0000H.
        ld      hl,(qcp_engine_source)
        call    qcpx_set_source
        xor     a
        ld      (qcp_dest_offset),a
        ld      (qcp_dest_offset+1),a
        ld      bc,(qcp_engine_size)
        call    qcpx_copy_range

        ; Common template immediately follows the engine.
        ld      hl,(qcp_common_source)
        call    qcpx_set_source
        ld      hl,(qcp_engine_size)
        ld      (qcp_dest_offset),hl
        ld      bc,(qcp_common_size)
        call    qcpx_copy_range

        ; Locate this page's record.  Each record is:
        ;   data_size, patch_count, patch_count*(offset,value), data.
        ld      hl,(qcp_records_source)
        ld      (qcp_cursor),hl
        ld      a,(qcp_page_index)
        ld      (qcp_page_index),a
qcpx_record_scan:
        ld      hl,(qcp_cursor)
        call    qcpx_set_source
        call    qcpx_read_word
        ld      (qcp_data_size),hl
        call    qcpx_read_word
        ld      (qcp_patch_count),hl
        ld      a,(qcp_page_index)
        or      a
        jr      z,qcpx_record_found
        ; cursor += 4 + (patch_count * 4) + data_size
        ld      hl,(qcp_patch_count)
        add     hl,hl
        add     hl,hl
        ld      de,4
        add     hl,de
        ld      de,(qcp_data_size)
        add     hl,de
        ld      de,(qcp_cursor)
        add     hl,de
        ld      (qcp_cursor),hl
        ld      a,(qcp_page_index)
        dec     a
        ld      (qcp_page_index),a
        jr      qcpx_record_scan

qcpx_record_found:
        ; Payload follows the fixed header and patch table.
        ld      hl,(qcp_cursor)
        ld      de,4
        add     hl,de
        ld      de,(qcp_patch_count)
        ex      de,hl
        add     hl,hl
        add     hl,hl
        ex      de,hl
        add     hl,de
        call    qcpx_set_source
        ld      hl,(qcp_page_data_address)
        ld      (qcp_dest_offset),hl
        ld      bc,(qcp_data_size)
        call    qcpx_copy_range

        ; Patch relocated common pointers into this materialized page.
        ld      hl,(qcp_cursor)
        ld      de,4
        add     hl,de
        call    qcpx_set_source
        call    qcpx_apply_patches
        or      a
        ret

qcpx_apply_patches:
        ld      hl,(qcp_patch_count)
        ld      (qcp_patch_left),hl
qcpx_patch_loop:
        ld      hl,(qcp_patch_left)
        ld      a,h
        or      l
        jr      z,qcpx_patch_done
        call    qcpx_read_word
        ld      (qcp_patch_offset),hl
        call    qcpx_read_word
        ld      (qcp_patch_value),hl
        ld      a,(qcp_dest_segment)
        call    PUT_P1_DISPATCH
        ld      hl,(qcp_patch_offset)
        ld      de,(qcp_engine_size)
        add     hl,de
        ld      de,0x4000
        add     hl,de
        ld      de,(qcp_patch_value)
        ld      (hl),e
        inc     hl
        ld      (hl),d
        ld      hl,(qcp_patch_left)
        dec     hl
        ld      (qcp_patch_left),hl
        jr      qcpx_patch_loop
qcpx_patch_done:
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ret

; Copy a range from the staged file to the selected physical page-1 segment.
; The source position and destination offset are set by the caller, and BC
; contains the range length.  A 0400H scratch buffer avoids aliasing the two
; page-1 mappings.
qcpx_copy_range:
        ld      (qcp_remaining),bc
qcpx_copy_loop:
        ld      hl,(qcp_remaining)
        ld      a,h
        or      l
        jp      z,qcpx_copy_done
        ld      bc,(qcp_remaining)
        call    source_boundary
        call    cap_bc_hl
        ld      (qcp_chunk_size),bc
        ld      hl,(qcp_dest_offset)
        ld      a,h
        and     0x3F
        ld      h,a
        ld      de,0x4000
        ex      de,hl
        ld      bc,(qcp_chunk_size)
        call    cap_bc_hl
        ld      (qcp_chunk_size),bc
        ; Never use more than the fixed page-3 scratch buffer.
        ld      hl,0x0400
        ld      bc,(qcp_chunk_size)
        call    cap_bc_hl
        ld      (qcp_chunk_size),bc

        call    map_source_page1
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      de,QCPX_SCRATCH
        ld      bc,(qcp_chunk_size)
        ldir

        ld      a,(qcp_dest_segment)
        call    PUT_P1_DISPATCH
        ld      hl,(qcp_dest_offset)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ex      de,hl
        ld      hl,QCPX_SCRATCH
        ld      bc,(qcp_chunk_size)
        ldir

        ld      hl,(qcp_chunk_size)
        ld      (chunk_size),hl
        call    advance_source
        ld      hl,(qcp_dest_offset)
        ld      de,(qcp_chunk_size)
        add     hl,de
        ld      (qcp_dest_offset),hl
        ld      hl,(qcp_remaining)
        ld      de,(qcp_chunk_size)
        or      a
        sbc     hl,de
        ld      (qcp_remaining),hl
        jp      qcpx_copy_loop
qcpx_copy_done:
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ret

; Standard forward ZX0 decoder. HL is a compressed stream in staged page 2;
; DE is the destination in the persistent page-1 complete page.
; QCPZ_DECODER_BEGIN
qcpz_zx0_decoder:
        ld      bc,0xFFFF
        push    bc
        inc     bc
        ld      a,0x80
qcpz_zx0_literals:
        call    qcpz_zx0_elias
        ldir
        add     a,a
        jr      c,qcpz_zx0_new_offset
        call    qcpz_zx0_elias
qcpz_zx0_copy:
        ex      (sp),hl
        push    hl
        add     hl,de
        ldir
        pop     hl
        ex      (sp),hl
        add     a,a
        jr      nc,qcpz_zx0_literals
qcpz_zx0_new_offset:
        pop     bc
        ld      c,0xFE
        call    qcpz_zx0_elias_loop
        inc     c
        ret     z
        ld      b,c
        ld      c,(hl)
        inc     hl
        rr      b
        rr      c
        push    bc
        ld      bc,1
        call    nc,qcpz_zx0_elias_backtrack
        inc     bc
        jr      qcpz_zx0_copy
qcpz_zx0_elias:
        inc     c
qcpz_zx0_elias_loop:
        add     a,a
        jr      nz,qcpz_zx0_elias_skip
        ld      a,(hl)
        inc     hl
        rla
qcpz_zx0_elias_skip:
        ret     c
qcpz_zx0_elias_backtrack:
        add     a,a
        rl      c
        rl      b
        jr      qcpz_zx0_elias_loop
; QCPZ_DECODER_END

; The complete-page Quarth image has a one-byte load image at file offset
; 20H and the QCPX descriptor immediately after it at 21H.  Probe that
; descriptor without using the normal KSSX extra-data arithmetic: QCPX is a
; deliberately self-describing container and is materialized by the DOS2
; player rather than copied as one conventional KSS image.
probe_qcpx:
        xor     a
        ld      (qcp_format),a
        ld      a,(header+2)
        cp      'S'
        ret     nz
        ld      a,(RUNTIME_STAGE_TABLE)
        call    PUT_P1_DISPATCH
        ld      a,(0x4021)
        cp      'Q'
        jr      nz,probe_qcpx_restore
        ld      a,(0x4022)
        cp      'C'
        jr      nz,probe_qcpx_restore
        ld      a,(0x4023)
        cp      'P'
        jr      nz,probe_qcpx_restore
        ld      a,(0x4024)
        cp      'X'
        jr      z,probe_qcpx_found
        cp      'Z'
        jr      nz,probe_qcpx_restore
        ld      a,2
        ld      (qcp_format),a
        jr      probe_qcpx_restore
probe_qcpx_found:
        ld      a,1
        ld      (qcp_format),a
probe_qcpx_restore:
        ld      a,(RUNTIME_BASIC_P1)
        call    PUT_P1_DISPATCH
        ret

player_end:
