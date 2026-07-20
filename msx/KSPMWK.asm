; MSX-DOS2 compact-KSP MWK uploader.
; Generated KSPMWK_SYMBOLS.inc supplies addresses in the resident page-3 player.

        include 'KSPMWK_SYMBOLS.inc'
        org     0x8000

BDOS:   equ     0x0005
SEEK:   equ     0x4A
READ:   equ     0x48

KSP_MWK_FLAGS:       equ 0x8400
KSP_MWK_INPUT:       equ KSP_MWK_FLAGS+64
KSP_MWK_OUTPUT:      equ KSP_MWK_INPUT+13

ksp_mwk_uploader_start:
; Upload an optional MoonBlaster MWK wavekit before MBWave INIT. In normal
; mode the kit is read from staged DOS2 mapper segments; compact direct mode
; refills a 16 KiB DOS buffer and sends sample bytes straight to OPL4 RAM.
        ld      a,(ksp_mwk_required)
        or      a
        ret     z
        ld      hl,ksp_mwk_offset
        call    ksp_set_source_from_hl
        ld      hl,(ksp_mwk_size)
        ld      (extra_position),hl
        ld      a,(ksp_mwk_size+2)
        ld      (extra_position+2),a
        ld      a,(RUNTIME_SPARSE_MODE)
        cp      2
        jr      z,ksp_mwk_direct_source
        call    map_source_page1
        call    ksp_mwk_refresh_pointer
        jr      ksp_mwk_source_ready
ksp_mwk_direct_source:
        xor     a
        ld      (field_remaining),a
        ld      (field_remaining+1),a
ksp_mwk_source_ready:

        ld      hl,ksp_mwk_signature
        ld      b,5
ksp_mwk_signature_loop:
        push    bc
        push    hl
        call    ksp_mwk_read_byte
        pop     hl
        pop     bc
        jp      c,ksp_mwk_fail
        cp      (hl)
        jp      nz,ksp_mwk_fail
        inc     hl
        djnz    ksp_mwk_signature_loop
        call    ksp_mwk_read_byte
        jp      c,ksp_mwk_fail
        cp      0x0C
        jr      z,ksp_mwk_edit
        cp      0x0D
        jp      nz,ksp_mwk_fail
        xor     a
        jr      ksp_mwk_mode_ready
ksp_mwk_edit:
        ld      a,1
ksp_mwk_mode_ready:
        ld      (fast_8k),a

        ; Declared sample size is advisory; actual reads remain chunk-bounded.
        ld      b,3
ksp_mwk_skip_declared:
        push    bc
        call    ksp_mwk_read_byte
        pop     bc
        jp      c,ksp_mwk_fail
        djnz    ksp_mwk_skip_declared
        call    ksp_mwk_read_byte
        jp      c,ksp_mwk_fail
        cp      49
        jp      nc,ksp_mwk_fail
        ld      (bank_count),a

        ld      hl,KSP_MWK_FLAGS
        ld      b,64
ksp_mwk_flags_loop:
        push    bc
        push    hl
        call    ksp_mwk_read_byte
        pop     hl
        pop     bc
        jp      c,ksp_mwk_fail
        ld      (hl),a
        inc     hl
        djnz    ksp_mwk_flags_loop

        ; Skip nr_of_waves OWN_PATCH records (25 bytes each).
        ld      a,(bank_count)
        or      a
        jr      z,ksp_mwk_patch_records_done
        ld      b,a
ksp_mwk_skip_patch_loop:
        ld      hl,25
        push    bc
        call    ksp_mwk_skip_hl
        pop     bc
        jp      c,ksp_mwk_fail
        djnz    ksp_mwk_skip_patch_loop
ksp_mwk_patch_records_done:
        ld      a,(fast_8k)
        or      a
        jr      z,ksp_mwk_tables_done
        ld      a,(bank_count)
        or      a
        jr      z,ksp_mwk_tables_done
        ld      b,a
ksp_mwk_skip_edit_patch_loop:
        ld      hl,16
        push    bc
        call    ksp_mwk_skip_hl
        pop     bc
        jp      c,ksp_mwk_fail
        djnz    ksp_mwk_skip_edit_patch_loop
ksp_mwk_tables_done:
        ; Permit custom wave headers and CPU writes to MoonSound sample RAM.
        ld      a,2
        ld      e,0x11
        call    ksp_opl4_write_reg
        ld      hl,0x0300
        ld      (check_position),hl
        ld      a,0x20
        ld      (check_position+2),a
        xor     a
        ld      hl,0
        ld      (destination),hl
        ld      hl,KSP_MWK_FLAGS
        ld      (kcp_patch_offset),hl
        ld      a,64
        ld      (bank_index),a

ksp_mwk_tone_loop:
        ld      hl,(kcp_patch_offset)
        ld      a,(hl)
        inc     hl
        ld      (kcp_patch_offset),hl
        ld      (bank_mode),a
        and     1
        jp      z,ksp_mwk_next_tone
        ld      a,(fast_8k)
        or      a
        jr      z,ksp_mwk_read_sample_header
        ld      hl,16
        call    ksp_mwk_skip_hl
        jp      c,ksp_mwk_fail_enabled
ksp_mwk_read_sample_header:
        ld      hl,KSP_MWK_INPUT
        ld      b,13
ksp_mwk_sample_header_loop:
        push    bc
        push    hl
        call    ksp_mwk_read_byte
        pop     hl
        pop     bc
        jp      c,ksp_mwk_fail_enabled
        ld      (hl),a
        inc     hl
        djnz    ksp_mwk_sample_header_loop

        ; Header bytes 1 and 2 always point at the next free RAM address.
        ld      a,(check_position+1)
        ld      (KSP_MWK_OUTPUT+1),a
        ld      a,(check_position+0)
        ld      (KSP_MWK_OUTPUT+2),a
        ld      a,(bank_mode)
        and     0x20
        jr      nz,ksp_mwk_rom_header
        ld      a,(check_position+2)
        and     0x3F
        ld      b,a
        ld      a,(bank_mode)
        and     0xC0
        or      b
        ld      (KSP_MWK_OUTPUT+0),a
        ld      hl,(KSP_MWK_INPUT+11)
        ld      (chunk_size),hl
        jr      ksp_mwk_header_tail
ksp_mwk_rom_header:
        ld      a,(KSP_MWK_INPUT+12)
        ld      b,a
        ld      a,(bank_mode)
        and     0xC0
        or      b
        ld      (KSP_MWK_OUTPUT+0),a
        ld      hl,0
        ld      (chunk_size),hl
ksp_mwk_header_tail:
        ld      hl,KSP_MWK_INPUT+2
        ld      de,KSP_MWK_OUTPUT+3
        ld      bc,9
        ldir

        ; Store this 12-byte OPL4 tone header at 200000H + tone*12.
        ld      hl,(destination)
        ld      a,0x20
        call    ksp_opl4_set_address
        ld      hl,KSP_MWK_OUTPUT
        ld      b,12
ksp_mwk_write_header_loop:
        ld      a,(hl)
        out     (0x7F),a
        inc     hl
        djnz    ksp_mwk_write_header_loop
        ld      hl,(chunk_size)
        ld      a,h
        or      l
        jp      z,ksp_mwk_next_tone
        ld      hl,(check_position)
        ld      a,(check_position+2)
        call    ksp_opl4_set_address
        ld      hl,(chunk_size)
ksp_mwk_sample_loop:
        ld      a,h
        or      l
        jr      z,ksp_mwk_sample_done
        push    hl
        call    ksp_mwk_read_byte
        pop     hl
        jp      c,ksp_mwk_fail_enabled
        out     (0x7F),a
        dec     hl
        jr      ksp_mwk_sample_loop
ksp_mwk_sample_done:
        ld      hl,(check_position)
        ld      de,(chunk_size)
        add     hl,de
        ld      (check_position),hl
        ld      a,(check_position+2)
        adc     a,0
        ld      (check_position+2),a
        cp      0x40
        jr      c,ksp_mwk_next_tone
        jp      nz,ksp_mwk_fail_enabled
        ld      a,h
        or      l
        jp      nz,ksp_mwk_fail_enabled
ksp_mwk_next_tone:
        ; Header slots are indexed by tone number, including inactive tones.
        ld      hl,(destination)
        ld      de,12
        add     hl,de
        ld      (destination),hl
        ld      a,(bank_index)
        dec     a
        ld      (bank_index),a
        jp      nz,ksp_mwk_tone_loop
        call    ksp_mwk_disable_write
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P1_DISPATCH
        or      a
        ret

ksp_mwk_fail_enabled:
        call    ksp_mwk_disable_write
ksp_mwk_fail:
        ld      a,(RUNTIME_KSS_TABLE)
        call    PUT_P1_DISPATCH
        scf
        ret

ksp_mwk_disable_write:
        ld      a,2
        ld      e,0x10
        call    ksp_opl4_write_reg
        ret

ksp_mwk_skip_hl:
        ld      a,h
        or      l
        ret     z
        push    hl
        call    ksp_mwk_read_byte
        pop     hl
        ret     c
        dec     hl
        jr      ksp_mwk_skip_hl

; Read one byte from a staged MWK without remapping page 1 for every byte.
ksp_mwk_read_byte:
        ld      a,(RUNTIME_SPARSE_MODE)
        cp      2
        jp      z,ksp_mwk_read_direct_byte
        ld      hl,(extra_position)
        ld      a,h
        or      l
        jr      nz,ksp_mwk_byte_available
        ld      a,(extra_position+2)
        or      a
        jr      z,ksp_mwk_read_eof
ksp_mwk_byte_available:
        ld      hl,(field_source)
        ld      a,(hl)
        ld      (kcp_value),a
        inc     hl
        ld      (field_source),hl
        ld      hl,(source_position)
        inc     hl
        ld      (source_position),hl
        ld      a,h
        or      l
        jr      nz,ksp_mwk_source_advanced
        ld      a,(source_position+2)
        inc     a
        ld      (source_position+2),a
ksp_mwk_source_advanced:
        ld      hl,(extra_position)
        ld      a,h
        or      l
        jr      nz,ksp_mwk_remaining_low
        ld      a,(extra_position+2)
        dec     a
        ld      (extra_position+2),a
        ld      hl,0xFFFF
        ld      (extra_position),hl
        jr      ksp_mwk_remaining_done
ksp_mwk_remaining_low:
        dec     hl
        ld      (extra_position),hl
ksp_mwk_remaining_done:
        ld      hl,(field_source)
        ld      a,h
        cp      0x80
        jr      nz,ksp_mwk_return_byte
        ld      hl,(extra_position)
        ld      a,h
        or      l
        ld      b,a
        ld      a,(extra_position+2)
        or      b
        jr      z,ksp_mwk_return_byte
        call    map_source_page1
        call    ksp_mwk_refresh_pointer
ksp_mwk_return_byte:
        ld      a,(kcp_value)
        or      a
        ret
ksp_mwk_read_eof:
        scf
        ret

; Direct compact-KSP mode. Page 1 is a 16 KiB DOS read buffer backed by the
; first KSS/runtime segment. The engine and song are materialized again after
; the MWK transfer, so overwriting that temporary page is harmless.
ksp_mwk_read_direct_byte:
        ld      hl,(field_remaining)
        ld      a,h
        or      l
        call    z,ksp_mwk_direct_fill
        ret     c
        ld      hl,(field_source)
        ld      a,(hl)
        ld      (kcp_value),a
        inc     hl
        ld      (field_source),hl
        ld      hl,(field_remaining)
        dec     hl
        ld      (field_remaining),hl

        ld      hl,(source_position)
        inc     hl
        ld      (source_position),hl
        ld      a,h
        or      l
        jr      nz,ksp_mwk_direct_source_advanced
        ld      a,(source_position+2)
        inc     a
        ld      (source_position+2),a
ksp_mwk_direct_source_advanced:
        ld      hl,(extra_position)
        ld      a,h
        or      l
        jr      nz,ksp_mwk_direct_remaining_low
        ld      a,(extra_position+2)
        dec     a
        ld      (extra_position+2),a
        ld      hl,0xFFFF
        ld      (extra_position),hl
        jr      ksp_mwk_direct_return
ksp_mwk_direct_remaining_low:
        dec     hl
        ld      (extra_position),hl
ksp_mwk_direct_return:
        ld      a,(kcp_value)
        or      a
        ret

ksp_mwk_direct_fill:
        xor     a
        ld      (ksp_mwk_direct_final),a
        ld      a,(extra_position+2)
        or      a
        jr      nz,ksp_mwk_direct_full_chunk
        ld      hl,(extra_position)
        ld      a,h
        or      l
        jr      z,ksp_mwk_direct_eof
        ld      a,h
        cp      0x40
        jr      c,ksp_mwk_direct_chunk_ready
        jr      nz,ksp_mwk_direct_full_chunk
        ld      a,l
        or      a
        jr      nz,ksp_mwk_direct_full_chunk
        ld      a,1
        ld      (ksp_mwk_direct_final),a
        jr      ksp_mwk_direct_full_chunk
ksp_mwk_direct_chunk_final:
        ld      a,1
        ld      (ksp_mwk_direct_final),a
        jr      ksp_mwk_direct_chunk_ready
ksp_mwk_direct_full_chunk:
        ld      hl,0x4000
ksp_mwk_direct_chunk_ready:
        ld      a,(extra_position+2)
        or      a
        jr      nz,ksp_mwk_direct_chunk_size_ready
        ld      a,h
        cp      0x40
        jr      nc,ksp_mwk_direct_chunk_size_ready
        jp      ksp_mwk_direct_chunk_final
ksp_mwk_direct_chunk_size_ready:
        ld      (field_remaining),hl
        ; The uploader itself executes from KSS segment 2 in page 2. Never
        ; map that same physical segment as the page-1 DOS buffer: READ would
        ; overwrite the running uploader. Reuse compact-KSP staging cache line
        ; 1 instead; engine and SONG are materialized only after this upload.
        ld      a,(RUNTIME_STAGE_TABLE+1)
        call    PUT_P1_DISPATCH

        ld      a,(RUNTIME_FILE_HANDLE)
        ld      b,a
        xor     a
        ld      d,0
        ld      a,(source_position+2)
        ld      e,a
        xor     a
        ld      hl,(source_position)
        ld      c,SEEK
        call    BDOS
        jr      nz,ksp_mwk_direct_fail

        ld      a,(RUNTIME_FILE_HANDLE)
        ld      b,a
        ld      de,0x4000
        ld      hl,(field_remaining)
        ld      c,READ
        call    BDOS
        jr      z,ksp_mwk_direct_read_ok
        cp      0xC7
        jr      nz,ksp_mwk_direct_fail
        ld      a,(ksp_mwk_direct_final)
        or      a
        jr      z,ksp_mwk_direct_fail
ksp_mwk_direct_read_ok:
        ld      hl,0x4000
        ld      (field_source),hl
        or      a
        ret
ksp_mwk_direct_eof:
ksp_mwk_direct_fail:
        scf
        ret

ksp_mwk_refresh_pointer:
        ld      hl,(source_position)
        ld      a,h
        and     0x3F
        or      0x40
        ld      h,a
        ld      (field_source),hl
        ret

; Set the OPL4 memory address in A:HL and select data register 06H.
ksp_opl4_set_address:
        ld      d,a
        and     0x3F
        ld      e,a
        ld      a,3
        call    ksp_opl4_write_reg
        ld      e,h
        ld      a,4
        call    ksp_opl4_write_reg
        ld      e,l
        ld      a,5
        call    ksp_opl4_write_reg
        ld      a,6
        out     (0x7E),a
        call    ksp_opl4_wait
        ret

; A=wave register, E=value.
ksp_opl4_write_reg:
        out     (0x7E),a
        call    ksp_opl4_wait
        ld      a,e
        out     (0x7F),a
        call    ksp_opl4_wait
        ret

ksp_opl4_wait:
        push    bc
        ld      b,8
ksp_opl4_wait_loop:
        in      a,(0x7E)
        djnz    ksp_opl4_wait_loop
        pop     bc
        ret

ksp_mwk_signature:
        defm    "MBMS"
        defb    0x10
ksp_mwk_uploader_end:
