; MSX-DOS2 generic KCPX/KCPZ player loader.
;
; The build script appends the fixed C000H player bootstrap.
; This COM program opens the user-supplied file through MSX-DOS2, allocates
; primary mapper segments with ALL_SEG, reads the raw file into allocated
; staging segments, and then transfers control to the resident player. Generic
; KCP containers materialize the selected engine/music image directly in
; page 1. Raw KSS/KSSX playback is intentionally unsupported.
;
; Usage: KSPPLAY.COM FILE.KSP [SONG]

        org     0x0100

BDOS:           equ     0x0005
TERM0:          equ     0x00
OPEN:           equ     0x43
CLOSE:          equ     0x45
READ:           equ     0x48
WRITE:          equ     0x49
SEEK:           equ     0x4A
EOF_ERROR:      equ     0xC7
TERM:           equ     0x62
EXTBIO:         equ     0xFFCA

KSS_SIZE:       equ     0x7FF3
KSS_STATE:      equ     0x7FF6
KSS_SONG:       equ     0x7FF7
; Keep the fixed KSS/player banks 7..11 and 14..15 inside a 256K mapper.
; The raw file staging area therefore starts at 12.
STAGE_BANK:     equ     12
HEADER_BUFFER:  equ     0x8000

PLAYER_RUNTIME_CONFIG: equ 0xC960
PLAYER_RUNTIME_STAGE:  equ PLAYER_RUNTIME_CONFIG
PLAYER_RUNTIME_MAIN:   equ PLAYER_RUNTIME_STAGE+32
PLAYER_RUNTIME_BASIC1: equ PLAYER_RUNTIME_MAIN+4
PLAYER_RUNTIME_BASIC2: equ PLAYER_RUNTIME_BASIC1+1
PLAYER_RUNTIME_KSS:    equ PLAYER_RUNTIME_BASIC2+1
PLAYER_RUNTIME_ORIGINAL_P2: equ PLAYER_RUNTIME_KSS+32
PLAYER_RUNTIME_PUT_P0: equ PLAYER_RUNTIME_ORIGINAL_P2+1
PLAYER_RUNTIME_PUT_P1: equ PLAYER_RUNTIME_PUT_P0+2
PLAYER_RUNTIME_PUT_P2: equ PLAYER_RUNTIME_PUT_P1+2
PLAYER_RUNTIME_STACK_TOP: equ PLAYER_RUNTIME_PUT_P2+2
PLAYER_RUNTIME_INPUT_FORMAT: equ PLAYER_RUNTIME_STACK_TOP+2
PLAYER_RUNTIME_SPARSE_MODE: equ PLAYER_RUNTIME_INPUT_FORMAT+1
PLAYER_RUNTIME_RELOAD_ENTRY: equ PLAYER_RUNTIME_SPARSE_MODE+1
; These bytes are in the unused fixed page-3 tail of the bootstrap.  The
; optional fourth command-line argument selects the SCC slot ID (decimal).
PLAYER_RUNTIME_SCC_SLOT:       equ 0xD2F0
PLAYER_RUNTIME_SCC_SLOT_VALID: equ 0xD2F1
SCC_SLOT_DEFAULT: equ 0x02
SCC_SLOT_MAGIC:   equ 0xA5

start:
        call    parse_command_line
        jp      c,usage_error
        ld      de,msg_01
        ld      hl,msg_01_end-msg_01
        ld      b,1
        ld      c,WRITE
        call    BDOS

        ld      de,msg_02a
        ld      hl,msg_02a_end-msg_02a
        ld      b,1
        ld      c,WRITE
        call    BDOS
        ; EXTBIO requires a page-3 stack.  Use the live TPA-top value supplied
        ; by DOS2 at 0006H; it changes with the installed cartridges.
        ld      hl,(0x0006)
        ld      sp,hl
        call    get_mapper_routines
        jp      c,mapper_extbio_error
        ld      de,msg_02b
        ld      hl,msg_02b_end-msg_02b
        ld      b,1
        ld      c,WRITE
        call    BDOS
        ld      de,msg_02c
        call    print_text
        ld      a,(mapper_free)
        call    print_hex_a
        call    capture_tpa_segments
        ld      de,msg_02
        call    print_text

        ld      de,filename
        xor     a                   ; read/write handle, no write restriction
        ld      c,OPEN
        call    BDOS
        jp      nz,dos_error
        ld      a,b
        ld      (file_handle),a
        ld      de,msg_03
        call    print_text

        ; Get the physical file size with SEEK relative to EOF.
        ld      a,(file_handle)
        ld      b,a
        ld      a,2
        ld      de,0
        ld      hl,0
        ld      c,SEEK
        call    BDOS
        jp      nz,dos_error
        ld      a,d
        or      a
        jp      nz,file_too_large
        ld      (file_size),hl
        ld      a,e
        ld      (file_size+2),a
        ld      de,msg_04
        call    print_text

        ; Read the header once before allocating the staging area.
        call    seek_start
        jp      nz,dos_error
        ld      a,(file_handle)
        ld      b,a
        ; Read directly into the loader's TPA header buffer.  This avoids
        ; depending on the current page-2 mapping before staging is set up.
        ld      de,header
        ; Read five bytes beyond the fixed KSSX header as a cheap KCPX/KCPZ
        ; signature probe. The container starts with its one-byte load image
        ; at 20H and its four-byte magic at 21H.
        ld      hl,0x25
        ld      c,READ
        call    BDOS
        jp      nz,dos_error
        ; Diagnostic: echo the four-byte magic actually copied from DOS2.
        ld      de,msg_04a
        call    print_text
        ld      de,header
        ld      hl,4
        ld      b,1
        ld      c,WRITE
        call    BDOS
        ld      de,msg_04c
        call    print_text
        ld      a,(header+0x0C)
        call    print_hex_a
        ld      de,msg_04d
        call    print_text
        ld      a,(header+0x0D)
        call    print_hex_a
        ld      de,msg_04e
        call    print_text
        ld      a,(header+5)
        call    print_hex_a
        ld      a,(header+4)
        call    print_hex_a
        ld      de,msg_04b
        call    print_text
        call    inspect_header
        jp      c,format_error
        ld      de,msg_05
        call    print_text

        ld      de,msg_loading
        call    print_text

        call    calculate_stage_count
        jp      c,file_too_large
        call    choose_staging_mode
        jp      c,mapper_error
        ld      de,msg_05a
        call    print_text
        ld      a,(stage_count)
        call    print_hex_a
        call    allocate_fixed_segments
        jp      c,mapper_error
        ld      de,msg_alloc
        call    print_text
        ld      de,msg_alloc_stage
        call    print_text
        ld      hl,stage_segments
        ld      a,(sparse_mode)
        or      a
        jr      z,print_alloc_stage_ready
        ld      hl,stage_pool
print_alloc_stage_ready:
        push    hl
        ld      a,(hl)
        call    print_hex_a
        pop     hl
        inc     hl
        ld      a,(hl)
        call    print_hex_a
        ld      de,msg_alloc_kss
        call    print_text
        ld      a,(kss_segments)
        call    print_hex_a
        ld      de,msg_alloc_main
        call    print_text
        ld      a,(main_segments)
        call    print_hex_a
        ld      a,(main_segments+1)
        call    print_hex_a
        ld      a,(main_segments+2)
        call    print_hex_a
        ld      a,(main_segments+3)
        call    print_hex_a
        ld      de,msg_06
        call    print_text

        ld      de,msg_06a
        call    print_text
        call    seek_start
        jp      nz,stage_seek_error
        ld      de,msg_06b
        call    print_text
        call    stage_file
        jp      nz,stage_read_error

        ; Titles already live in the containers: INFO records in KCP files
        ; and title= in the compact KSP META chunk.
        call    print_selected_title

        ld      a,(sparse_mode)
        or      a
        jr      nz,patch_file_done
        ld      a,(file_handle)
        ld      b,a
        ld      c,CLOSE
        call    BDOS
        jp      nz,dos_error

patch_file_done:
        ld      de,msg_07
        call    print_text
        ld      de,msg_loaded
        call    print_text

        ; No DOS calls are made after this point.  Hand the existing player
        ; its normal BASIC-compatible state and let it take over C000H.
        ld      de,msg_08
        call    print_text
launch_player:
        ; DOS2 console output is allowed to use the top of the TPA.  In
        ; particular, it can overwrite 7FF0H..7FF7H, so write the player
        ; handoff values only after the final BDOS call.
        ld      hl,(file_size)
        ld      (KSS_SIZE),hl
        ld      a,(file_size+2)
        ld      (KSS_SIZE+2),a
        ld      a,(song_number)
        ld      (KSS_SONG),a
        ld      a,1
        ld      (KSS_STATE),a
        ld      a,(scc_slot)
        ld      (PLAYER_RUNTIME_SCC_SLOT),a
        ld      a,SCC_SLOT_MAGIC
        ld      (PLAYER_RUNTIME_SCC_SLOT_VALID),a
        ld      hl,player_boot_blob
        ld      de,0xC000
        ld      bc,player_boot_blob_end-player_boot_blob
        ldir

        ; Install the dynamic mapper layout in the bootstrap configuration.
        ld      hl,stage_segments
        ld      de,PLAYER_RUNTIME_STAGE
        ld      bc,32
        ldir
        ld      hl,main_segments
        ld      de,PLAYER_RUNTIME_MAIN
        ld      bc,4
        ldir
        ld      a,(main_segments+1)
        ld      (PLAYER_RUNTIME_BASIC1),a
        ld      a,(main_segments+2)
        ld      (PLAYER_RUNTIME_BASIC2),a
        ld      hl,kss_segments
        ld      de,PLAYER_RUNTIME_KSS
        ld      bc,32
        ldir
        ld      a,(main_segments+2)
        ld      (PLAYER_RUNTIME_ORIGINAL_P2),a
        ld      hl,(0x0006)
        ld      (PLAYER_RUNTIME_STACK_TOP),hl
        ld      hl,(mapper_calls)
        ld      de,0x18          ; DOS2 PUT_P0
        add     hl,de
        ld      (PLAYER_RUNTIME_PUT_P0),hl
        ld      hl,(mapper_calls)
        ld      de,0x1E          ; DOS2 PUT_P1
        add     hl,de
        ld      (PLAYER_RUNTIME_PUT_P1),hl
        ld      hl,(mapper_calls)
        ld      de,0x24          ; DOS2 PUT_P2
        add     hl,de
        ld      (PLAYER_RUNTIME_PUT_P2),hl
        ld      a,(input_format)
        ld      (PLAYER_RUNTIME_INPUT_FORMAT),a
        ld      a,(sparse_mode)
        ld      (PLAYER_RUNTIME_SPARSE_MODE),a
        ld      hl,sparse_reload_entry
        ld      (PLAYER_RUNTIME_RELOAD_ENTRY),hl
        jp      0xC000

; The resident page-3 player returns here for a sparse-file track change.
; Page 0 still contains this loader, DOS mapper/slot state has been restored,
; and A is the new public song number. Refill only the selected source pages.
sparse_reload_entry:
        di
        ld      (song_number),a
        ld      hl,(0x0006)
        ld      sp,hl
        call    stage_file
        jp      nz,stage_read_error
        ; Do not rescan the on-disk INFO block here: a cache-hit track change
        ; must remain free of floppy I/O. The initial launch already printed
        ; the selected title.
        jp      launch_player

; Parse the first command-line token into filename and an optional decimal
; song number.  MSX-DOS places the tail length at 80H and text at 81H.
parse_command_line:
        ld      a,SCC_SLOT_DEFAULT
        ld      (scc_slot),a
        xor     a
        ld      (song_number),a
        ld      hl,0x0081
        ld      a,(0x0080)
        ld      b,a
parse_skip:
        ld      a,b
        or      a
        jp      z,parse_bad
        ld      a,(hl)
        cp      ' '
        jr      nz,parse_name
        inc     hl
        djnz    parse_skip
        jp      parse_bad
parse_name:
        ld      de,filename
parse_name_loop:
        ld      a,b
        or      a
        jr      z,parse_name_done
        ld      a,(hl)
        cp      ' '
        jr      z,parse_name_done
        cp      9
        jr      z,parse_name_done
        ld      (de),a
        inc     de
        inc     hl
        djnz    parse_name_loop
parse_name_done:
        xor     a
        ld      (de),a
        ld      a,b
        or      a
        jp      z,parse_good
parse_song_skip:
        ld      a,(hl)
        cp      ' '
        jr      nz,parse_song_start
        inc     hl
        djnz    parse_song_skip
        jr      parse_good
parse_song_start:
        xor     a
        ld      (song_number),a
parse_song_loop:
        ld      a,b
        or      a
        jr      z,parse_scc_slot_skip
        ld      a,(hl)
        cp      '0'
        jr      c,parse_bad
        cp      '9'+1
        jr      nc,parse_bad
        sub     '0'
        ld      c,a
        ld      a,(song_number)
        add     a,a
        ld      d,a
        add     a,a
        add     a,a
        add     a,d
        add     a,c
        ld      (song_number),a
        inc     hl
        djnz    parse_song_loop
parse_scc_slot_skip:
        ld      a,b
        or      a
        jr      z,parse_good
parse_scc_slot_spaces:
        ld      a,b
        or      a
        jr      z,parse_good
        ld      a,(hl)
        cp      ' '
        cp      13
        jr      z,parse_good
        cp      ' '
        jr      nz,parse_scc_slot_start
        inc     hl
        djnz    parse_scc_slot_spaces
        jr      parse_good
parse_scc_slot_start:
        xor     a
        ld      (scc_slot),a
parse_scc_slot_loop:
        ld      a,b
        or      a
        jr      z,parse_scc_slot_done
        ld      a,(hl)
        cp      ' '
        jr      z,parse_scc_slot_done
        cp      9
        jr      z,parse_scc_slot_done
        cp      13
        jr      z,parse_scc_slot_done
        cp      '0'
        jr      c,parse_bad
        cp      '9'+1
        jr      nc,parse_bad
        sub     '0'
        ld      c,a
        ld      a,(scc_slot)
        add     a,a
        ld      d,a
        add     a,a
        add     a,a
        add     a,d
        add     a,c
        ld      (scc_slot),a
        inc     hl
        djnz    parse_scc_slot_loop
parse_scc_slot_done:
        ld      a,(scc_slot)
        cp      4
        jr      c,parse_good
        and     0x80
        jr      z,parse_bad
        ld      a,(scc_slot)
        and     0x70
        jr      nz,parse_bad
parse_good:
        or      a
        ret
parse_bad:
        scf
        ret

get_mapper_routines:
        xor     a
        ld      d,4
        ld      e,2
        call    EXTBIO
        or      a
        jr      z,get_mapper_bad
        ld      (mapper_calls),hl
        ld      (mapper_total),a
        ld      a,c
        ld      (mapper_free),a
        or      a
        ret
get_mapper_bad:
        scf
        ret

capture_tpa_segments:
        ld      hl,(mapper_calls)
        ld      de,0x1B          ; GET_P0
        add     hl,de
        call    call_hl
        ld      (main_segments+0),a
        ld      hl,(mapper_calls)
        ld      de,0x21          ; GET_P1
        add     hl,de
        call    call_hl
        ld      (main_segments+1),a
        ld      hl,(mapper_calls)
        ld      de,0x27          ; GET_P2
        add     hl,de
        call    call_hl
        ld      (main_segments+2),a
        ld      hl,(mapper_calls)
        ld      de,0x2D          ; GET_P3
        add     hl,de
        call    call_hl
        ld      (main_segments+3),a
        ret

; Allocate only what the player needs and retain the physical segment IDs.
; On a 256K machine MSX-DOS2 commonly leaves six user segments available:
; two for raw staging, one for the relocated player, and one for F-1 banked
; data.
allocate_fixed_segments:
        ld      a,(sparse_mode)
        or      a
        jr      z,allocate_full_stage
        ld      ix,stage_pool
        ld      a,(stage_alloc_count)
        jr      allocate_stage_list
allocate_full_stage:
        ld      ix,stage_segments
        ld      a,(stage_count)
allocate_stage_list:
        ld      c,a
        call    allocate_list
        ret     c
        ld      ix,kss_segments
        ld      a,(declared_banks)
        ld      c,a
        call    allocate_list
        ret

allocate_list:
        ld      d,c
        ld      a,d
        or      a
        ret     z
allocate_list_loop:
        xor     a
        ld      b,0                 ; primary mapper
        call    mapper_all
        ret     c
        ld      (ix+0),a
        inc     ix
        dec     d
        jr      nz,allocate_list_loop
        or      a
        ret

mapper_all:
        ld      hl,(mapper_calls)
        call    call_hl
        ret

mapper_put_p2:
        ld      hl,(mapper_calls)
        ld      de,0x24
        add     hl,de
        call    call_hl
        ret

call_hl:
        jp      (hl)

seek_start:
        ld      a,(file_handle)
        ld      b,a
        xor     a
        ld      de,0
        ld      hl,0
        ld      c,SEEK
        call    BDOS
        ret

inspect_header:
        ld      a,(header)
        cp      'K'
        jr      nz,inspect_bad
        ld      a,(header+1)
        cp      'S'
        jr      nz,inspect_bad
        ld      a,(header+2)
        cp      'C'
        jr      z,inspect_kscc_magic
        cp      'S'
        jr      nz,inspect_bad
        ld      a,(header+3)
        cp      'X'
        jr      nz,inspect_bad
        jr      inspect_magic_ok
inspect_kscc_magic:
        ld      a,(header+3)
        cp      'C'
        jr      nz,inspect_bad
inspect_magic_ok:
        ; Generic complete-page containers always use the 16K KSS mode.
        ld      a,(header+0x0D)
        and     0x80
        jr      nz,inspect_bad
        call    probe_kcp_header
        ld      a,(kcp_file)
        or      a
        jr      nz,inspect_good
        call    probe_compact_ksp_file
        ret     c
inspect_good:
        or      a
        ret
inspect_bad:
        scf
        ret

probe_kcp_header:
        xor     a
        ld      (kcp_file),a
        ld      a,(header+2)
        cp      'S'
        ret     nz
        ld      a,(header+0x21)
        cp      'K'
        ret     nz
        ld      a,(header+0x22)
        cp      'C'
        ret     nz
        ld      a,(header+0x23)
        cp      'P'
        ret     nz
        ld      a,(header+0x24)
        cp      'X'
        jr      z,probe_kcp_header_raw
        cp      'Z'
        ret     nz
        ld      c,2                 ; destination plus overlay temp
        jr      probe_kcp_header_found
probe_kcp_header_raw:
        ld      c,1
probe_kcp_header_found:
        ld      a,1
        ld      (kcp_file),a
        ld      (input_format),a
        ld      a,c
        ld      (declared_banks),a
        ret

; Compact resource KSP files have no fixed marker at 21H. Validate their
; 24-byte KSP1 trailer directly from the still-open DOS2 file.
probe_compact_ksp_file:
        ld      a,(file_handle)
        ld      b,a
        ld      a,2                 ; seek relative to EOF
        ld      de,0xFFFF
        ld      hl,0xFFE8           ; -24 bytes
        ld      c,SEEK
        call    BDOS
        jr      nz,probe_compact_bad
        ld      a,(file_handle)
        ld      b,a
        ld      de,trailer
        ld      hl,24
        ld      c,READ
        call    BDOS
        jr      nz,probe_compact_bad
        ld      hl,trailer
        ld      de,ksp1_magic
        ld      b,4
probe_compact_magic_loop:
        ld      a,(de)
        cp      (hl)
        jr      nz,probe_compact_bad
        inc     de
        inc     hl
        djnz    probe_compact_magic_loop
        ld      hl,(trailer+4)
        ld      de,24
        or      a
        sbc     hl,de
        jr      nz,probe_compact_bad
        ld      a,2
        ld      (input_format),a
        inc     a
        ld      (declared_banks),a  ; engine, song and page-3 MBWave work
        or      a
        ret
probe_compact_bad:
        scf
        ret

calculate_stage_count:
        ; ceil(file_size / 4000H), using (size+3FFFH)>>14.
        ld      hl,(file_size)
        ld      a,(file_size+2)
        ld      de,0x3FFF
        add     hl,de
        adc     a,0
        ld      b,a
        ld      a,h
        and     0xC0
        rrca
        rrca
        rrca
        rrca
        rrca
        rrca
        ld      c,a
        ld      a,b
        add     a,a
        add     a,a
        add     a,c
        jr      z,calculate_bad
        cp      33
        jr      nc,calculate_bad
        ld      (stage_count),a
        add     a,STAGE_BANK
        jr      c,calculate_bad
        or      a
        ret
calculate_bad:
        scf
        ret

; Use whole-file staging whenever it fits. Large compressed KCP files can be
; staged sparsely because their template is in file segment 0 and each packed
; overlay occupies at most two additional 16K file segments. Use a fourth
; stage segment as a third cache line when DOS2 leaves enough mapper RAM.
choose_staging_mode:
        xor     a
        ld      (sparse_mode),a
        ld      (sparse_initialized),a
        ld      a,(stage_count)
        ld      b,a
        ld      a,(declared_banks)
        add     a,b
        ld      b,a
        ld      a,(mapper_free)
        cp      b
        jr      nc,choose_staging_full
        ld      a,(kcp_file)
        or      a
        jr      z,choose_staging_bad
        ld      a,(header+0x24)
        cp      'Z'
        jr      nz,choose_staging_bad
        ld      a,(declared_banks)
        add     a,3
        ld      b,a
        ld      a,(mapper_free)
        cp      b
        jr      c,choose_staging_bad
        ld      a,1
        ld      (sparse_mode),a
        ld      a,(mapper_free)
        ld      b,a
        ld      a,(declared_banks)
        ld      c,a
        ld      a,b
        sub     c
        cp      4
        jr      c,choose_staging_three
        ld      a,4
        jr      choose_staging_sparse_count
choose_staging_three:
        ld      a,3
choose_staging_sparse_count:
        ld      (stage_alloc_count),a
        dec     a
        ld      (sparse_cache_count),a
        or      a
        ret
choose_staging_full:
        ld      a,(stage_count)
        ld      (stage_alloc_count),a
        or      a
        ret
choose_staging_bad:
        scf
        ret

stage_file:
        ld      a,(sparse_mode)
        or      a
        jp      nz,stage_sparse_file
        xor     a
        ld      (stage_index),a
stage_loop:
        ld      a,(stage_index)
        ld      e,a
        ld      d,0
        ld      hl,stage_segments
        add     hl,de
        ld      a,(hl)
        call    mapper_put_p2
        ld      de,msg_06c
        call    print_text
        ld      a,(file_handle)
        ld      b,a
        ld      de,0x8000
        ld      hl,0x4000
        ld      c,READ
        call    BDOS
        or      a
        ret     nz
        ld      de,msg_06d
        call    print_text
        call    print_dot
        ld      a,(stage_index)
        inc     a
        ld      (stage_index),a
        ld      c,a
        ld      a,(stage_count)
        cp      c
        jr      nz,stage_loop
        xor     a
        ret

; Stage segment 0 plus the one or two 16K file segments containing the
; selected compressed overlay. Missing logical entries alias segment 0; the
; runtime only dereferences the template and selected descriptor ranges.
stage_sparse_file:
        ld      a,(stage_pool)
        ld      hl,stage_segments
        ld      b,32
stage_sparse_fill_table:
        ld      (hl),a
        inc     hl
        djnz    stage_sparse_fill_table

        ld      a,(sparse_initialized)
        or      a
        jr      nz,stage_sparse_map_header
        ld      a,0xFF
        ld      (sparse_cache_tags),a
        ld      (sparse_cache_tags+1),a
        ld      (sparse_cache_tags+2),a
        xor     a
        ld      (sparse_cache_next),a
        ld      c,a                 ; logical file segment 0 -> pool slot 0
        call    stage_sparse_segment
        ret     nz
        ld      a,1
        ld      (sparse_initialized),a
        jr      stage_sparse_header_ready
stage_sparse_map_header:
        ; Pool slot 0 is immutable after the first load. Re-expose it for
        ; descriptor parsing without touching the disk.
        ld      a,(stage_pool)
        call    mapper_put_p2
stage_sparse_header_ready:

        ; Translate public song ID through KCP's page map in segment 0.
        ld      a,(song_number)
        ld      e,a
        ld      d,0
        ld      hl,0x8131
        add     hl,de
        ld      a,(hl)
        cp      0xFF
        jp      z,stage_sparse_bad
        ld      e,a
        ld      d,0

        ; Overlay descriptors start at absolute file offset 0235H. Version 1
        ; records are six bytes; version 2 records are eight bytes.
        ld      hl,0
        ld      a,(0x8025)
        cp      2
        jr      z,stage_sparse_descriptor_v2
        ld      l,e
        ld      h,d
        add     hl,hl
        add     hl,de
        add     hl,hl              ; index * 6
        jr      stage_sparse_descriptor_ready
stage_sparse_descriptor_v2:
        ld      l,e
        ld      h,d
        add     hl,hl
        add     hl,hl
        add     hl,hl              ; index * 8
stage_sparse_descriptor_ready:
        ld      de,0x8235
        add     hl,de
        inc     hl
        inc     hl
        ld      e,(hl)             ; packed size
        inc     hl
        ld      d,(hl)
        inc     hl
        ld      (sparse_packed_size),de
        ld      e,(hl)             ; KCP-relative packed offset, low word
        inc     hl
        ld      d,(hl)
        inc     hl
        xor     a
        ld      (sparse_source_offset+2),a
        ld      a,(0x8025)
        cp      2
        jr      nz,stage_sparse_offset_ready
        ld      a,(hl)
        ld      (sparse_source_offset+2),a
        inc     hl
        ld      a,(hl)
        or      a
        jr      nz,stage_sparse_bad
stage_sparse_offset_ready:
        ex      de,hl
        ld      de,0x21
        add     hl,de
        ld      (sparse_source_offset),hl
        jr      nc,stage_sparse_offset_no_carry
        ld      a,(sparse_source_offset+2)
        inc     a
        ld      (sparse_source_offset+2),a
stage_sparse_offset_no_carry:
        call    sparse_offset_segment
        ld      (sparse_first_segment),a
        ld      (sparse_protected_segment),a
        call    stage_sparse_ensure_segment
        ret     nz
stage_sparse_first_loaded:
        ; Determine the segment containing the packed stream's final byte.
        ld      hl,(sparse_source_offset)
        ld      de,(sparse_packed_size)
        add     hl,de
        dec     hl
        ld      (sparse_end_offset),hl
        ld      a,(sparse_source_offset+2)
        adc     a,0
        ld      (sparse_end_offset+2),a
        ld      hl,sparse_end_offset
        call    sparse_pointer_segment
        ld      b,a
        ld      a,(sparse_first_segment)
        cp      b
        jr      z,stage_sparse_done
        inc     a
        cp      b
        jr      nz,stage_sparse_bad ; one overlay may span at most two pages
        ld      a,b
        call    stage_sparse_ensure_segment
        ret     nz
stage_sparse_done:
        ld      a,0xFF
        ld      (sparse_protected_segment),a
        xor     a
        ret
stage_sparse_bad:
        ld      a,1
        or      a
        ret

; Return source_offset >> 14 in A.
sparse_offset_segment:
        ld      hl,sparse_source_offset
sparse_pointer_segment:
        ld      a,(hl)
        inc     hl
        ld      a,(hl)
        and     0xC0
        rrca
        rrca
        rrca
        rrca
        rrca
        rrca
        ld      c,a
        inc     hl
        ld      a,(hl)
        add     a,a
        add     a,a
        add     a,c
        ret

; Ensure logical file segment A is resident in one of the music cache lines.
; Cache line 0 corresponds to stage_pool+1; stage_pool itself permanently
; contains file segment 0. A cache hit only repairs the logical stage table
; and performs no DOS seek/read.
stage_sparse_ensure_segment:
        or      a
        ret     z
        ld      (sparse_logical_segment),a
        ld      a,(sparse_cache_count)
        ld      b,a
        ld      hl,sparse_cache_tags
        xor     a
        ld      c,a
stage_sparse_cache_search:
        ld      a,(sparse_logical_segment)
        cp      (hl)
        jr      z,stage_sparse_cache_hit
        inc     hl
        inc     c
        djnz    stage_sparse_cache_search

        ; Round-robin replacement, skipping the first half of a currently
        ; selected two-segment stream.
        ld      a,(sparse_cache_next)
        ld      c,a
stage_sparse_choose_victim:
        ld      e,c
        ld      d,0
        ld      hl,sparse_cache_tags
        add     hl,de
        ld      a,(sparse_protected_segment)
        cp      (hl)
        jr      nz,stage_sparse_victim_ready
        inc     c
        ld      a,(sparse_cache_count)
        cp      c
        jr      nz,stage_sparse_choose_victim
        ld      c,0
        jr      stage_sparse_choose_victim
stage_sparse_victim_ready:
        ld      a,c
        ld      (sparse_cache_slot),a
        inc     a
        ld      b,a
        ld      a,(sparse_cache_count)
        cp      b
        jr      nz,stage_sparse_next_ready
        ld      b,0
stage_sparse_next_ready:
        ld      a,b
        ld      (sparse_cache_next),a

        ld      a,(sparse_cache_slot)
        inc     a                   ; pool slot 1..3
        ld      c,a
        ld      a,(sparse_logical_segment)
        call    stage_sparse_segment
        ret     nz
        ld      a,(sparse_cache_slot)
        ld      e,a
        ld      d,0
        ld      hl,sparse_cache_tags
        add     hl,de
        ld      a,(sparse_logical_segment)
        ld      (hl),a
        xor     a
        ret

stage_sparse_cache_hit:
        ld      a,c
        ld      e,a
        ld      d,0
        ld      hl,stage_pool+1
        add     hl,de
        ld      a,(hl)
        ld      (sparse_physical_segment),a
        ld      a,(sparse_logical_segment)
        ld      e,a
        ld      d,0
        ld      hl,stage_segments
        add     hl,de
        ld      a,(sparse_physical_segment)
        ld      (hl),a
        xor     a
        ret

; A = logical file segment, C = stage_pool index. The physical mapper ID is
; installed in the corresponding logical runtime-table entry.
stage_sparse_segment:
        ld      (sparse_logical_segment),a
        ld      a,c
        ld      e,a
        ld      d,0
        ld      hl,stage_pool
        add     hl,de
        ld      a,(hl)
        ld      (sparse_physical_segment),a
        call    mapper_put_p2
        ld      a,(sparse_logical_segment)
        ld      e,a
        ld      d,0
        ld      hl,stage_segments
        add     hl,de
        ld      a,(sparse_physical_segment)
        ld      (hl),a

        ; Seek to logical_segment * 4000H (32-bit DE:HL offset).
        ld      a,(sparse_logical_segment)
        ld      c,a
        and     3
        rrca
        rrca
        ld      h,a
        ld      l,0
        ld      a,c
        rrca
        rrca
        and     0x3F
        ld      e,a
        ld      d,0
        ld      a,(file_handle)
        ld      b,a
        xor     a
        ld      c,SEEK
        call    BDOS
        ret     nz
        ld      a,(file_handle)
        ld      b,a
        ld      de,0x8000
        ld      hl,0x4000
        ld      c,READ
        call    BDOS
        ret

print_text:
        ; Use DOS2 standard output handle 1 and function 49H.
        push    de
        ld      hl,0
        ex      de,hl
        ld      bc,0
print_text_length:
        ld      a,(hl)
        cp      '$'
        jr      z,print_text_write
        inc     hl
        inc     bc
        jr      print_text_length
print_text_write:
        pop     de
        ld      h,b
        ld      l,c
        ld      b,1
        ld      c,WRITE
        call    BDOS
        ret

print_hex_a:
        push    af
        rrca
        rrca
        rrca
        rrca
        and     0x0F
        call    hex_digit
        ld      (hex_buffer),a
        pop     af
        and     0x0F
        call    hex_digit
        ld      (hex_buffer+1),a
        ld      de,hex_buffer
        ld      hl,2
        ld      b,1
        ld      c,WRITE
        jp      BDOS

hex_digit:
        cp      10
        jr      c,hex_digit_number
        add     a,'A'-10
        ret
hex_digit_number:
        add     a,'0'
        ret

print_dot:
        ld      de,msg_dot
        jp      print_text

; -------------------------------------------------------------------------
; Track-title output while the source file is still open.

print_selected_title:
        ld      a,(input_format)
        cp      1
        jr      z,print_kcp_title
        cp      2
        jr      z,print_ksp_title
        ret

; KCP files append a standard KSS INFO block directly after the extended
; payload. Header word 10H gives that payload size.
print_kcp_title:
        ld      hl,(header+0x10)
        ld      de,0x20
        add     hl,de
        call    title_seek_hl
        ret     nz
        ld      de,title_scratch
        ld      hl,16
        call    title_read_exact
        ret     c
        ld      hl,title_scratch
        ld      de,info_magic
        ld      b,4
print_kcp_info_magic:
        ld      a,(de)
        cp      (hl)
        ret     nz
        inc     de
        inc     hl
        djnz    print_kcp_info_magic
        ld      a,(title_scratch+9)
        or      a
        ret     nz
        ld      a,(title_scratch+8)
        ld      (title_entries_left),a
print_kcp_record_loop:
        ld      a,(title_entries_left)
        or      a
        ret     z
        ld      de,title_scratch
        ld      hl,10
        call    title_read_exact
        ret     c
        ld      a,(title_scratch)
        ld      c,a
        ld      a,(song_number)
        cp      c
        jr      z,print_kcp_record_found
        call    title_skip_zero_string
        ret     c
        ld      a,(title_entries_left)
        dec     a
        ld      (title_entries_left),a
        jr      print_kcp_record_loop
print_kcp_record_found:
        ld      c,0
        call    title_read_string
        ret     c
        jp      print_title_buffer

; Compact KSP stores human-readable metadata in an uncompressed META entry.
; Locate META[0] in KDIR and read its leading title= line.
print_ksp_title:
        ld      a,(trailer+10)
        or      a
        ret     nz
        ld      a,(trailer+11)
        or      a
        ret     nz
        ld      hl,(trailer+8)
        call    title_seek_hl
        ret     nz
        ld      de,title_scratch
        ld      hl,16
        call    title_read_exact
        ret     c
        ld      hl,title_scratch
        ld      de,kdir_magic
        ld      b,4
print_ksp_kdir_magic:
        ld      a,(de)
        cp      (hl)
        ret     nz
        inc     de
        inc     hl
        djnz    print_ksp_kdir_magic
        ld      a,(title_scratch+9)
        or      a
        ret     nz
        ld      a,(title_scratch+10)
        or      a
        ret     nz
        ld      a,(title_scratch+11)
        or      a
        ret     nz
        ld      a,(title_scratch+8)
        ld      (title_entries_left),a
print_ksp_entry_loop:
        ld      a,(title_entries_left)
        or      a
        ret     z
        ld      de,title_scratch
        ld      hl,32
        call    title_read_exact
        ret     c
        ld      hl,title_scratch
        ld      de,meta_magic
        ld      b,4
print_ksp_meta_magic:
        ld      a,(de)
        cp      (hl)
        jr      nz,print_ksp_entry_next
        inc     de
        inc     hl
        djnz    print_ksp_meta_magic
        ld      hl,(title_scratch+4)
        ld      a,h
        or      l
        jr      nz,print_ksp_entry_next
        ld      hl,(title_scratch+6)
        ld      a,h
        or      l
        jr      nz,print_ksp_entry_next
        ld      hl,(title_scratch+24)
        ld      a,h
        or      l
        jr      nz,print_ksp_entry_next
        ld      a,(title_scratch+10)
        or      a
        ret     nz
        ld      a,(title_scratch+11)
        or      a
        ret     nz
        ld      hl,(title_scratch+8)
        call    title_seek_hl
        ret     nz
        ld      de,title_scratch
        ld      hl,6
        call    title_read_exact
        ret     c
        ld      hl,title_scratch
        ld      de,title_key
        ld      b,6
print_ksp_title_key:
        ld      a,(de)
        cp      (hl)
        ret     nz
        inc     de
        inc     hl
        djnz    print_ksp_title_key
        ld      c,10
        call    title_read_string
        ret     c
        jp      print_title_buffer
print_ksp_entry_next:
        ld      a,(title_entries_left)
        dec     a
        ld      (title_entries_left),a
        jr      print_ksp_entry_loop

title_skip_zero_string:
        ld      a,255
        ld      (title_skip_left),a
title_skip_zero_loop:
        call    title_read_byte
        ret     c
        or      a
        ret     z
        ld      a,(title_skip_left)
        dec     a
        ld      (title_skip_left),a
        jr      nz,title_skip_zero_loop
        scf
        ret

; Read into a 72-character display buffer until delimiter C. A zero byte
; also terminates META safely. Excess text is consumed, not wrapped.
title_read_string:
        ld      hl,title_buffer
        ld      a,c
        ld      (title_delimiter),a
        xor     a
        ld      (title_length),a
        ld      a,255
        ld      (title_skip_left),a
title_read_string_loop:
        push    hl
        call    title_read_byte
        pop     hl
        ret     c
        ld      b,a
        or      a
        jr      z,title_read_string_done
        ld      a,(title_delimiter)
        cp      b
        jr      z,title_read_string_done
        ld      a,(title_length)
        cp      72
        jr      nc,title_read_string_drop
        ld      a,b
        ld      (hl),a
        inc     hl
        ld      a,(title_length)
        inc     a
        ld      (title_length),a
        jr      title_read_string_count
title_read_string_drop:
title_read_string_count:
        ld      a,(title_skip_left)
        dec     a
        ld      (title_skip_left),a
        jr      nz,title_read_string_loop
        scf
        ret
title_read_string_done:
        or      a
        ret

print_title_buffer:
        ld      a,(title_length)
        or      a
        ret     z
        ld      de,msg_track
        call    print_text
        ld      de,title_buffer
        ld      a,(title_length)
        ld      l,a
        ld      h,0
        ld      b,1
        ld      c,WRITE
        call    BDOS
        ld      de,msg_crlf
        jp      print_text

title_seek_hl:
        ld      a,(file_handle)
        ld      b,a
        xor     a
        ld      de,0
        ld      c,SEEK
        jp      BDOS

title_read_exact:
        ld      a,(file_handle)
        ld      b,a
        ld      c,READ
        call    BDOS
        jr      nz,title_read_failed
        or      a
        ret
title_read_failed:
        scf
        ret

title_read_byte:
        ld      a,(file_handle)
        ld      b,a
        ld      de,title_byte
        ld      hl,1
        ld      c,READ
        call    BDOS
        jr      nz,title_read_failed
        ld      a,h
        or      l
        jr      z,title_read_failed
        ld      a,(title_byte)
        or      a
        ret

usage_error:
        ld      de,msg_usage
        jr      print_and_exit
format_error:
        ld      de,msg_format
        jr      print_and_exit
file_too_large:
        ld      de,msg_large
        jr      print_and_exit
mapper_error:
        ld      de,msg_mapper
        jr      print_and_exit
mapper_extbio_error:
        ld      de,msg_extbio
        jr      print_and_exit
dos_error:
        ld      de,msg_dos
print_and_exit:
        ; DOS2 standard output handle 1, function 49H.  Do not use the
        ; CP/M function 09H string printer in an MSX-DOS2 COM file.
        call    print_text
        xor     b
        ld      c,TERM0
        jp      BDOS

stage_seek_error:
        ld      de,msg_stage_seek
        call    print_text
        call    print_hex_a
        jr      terminate_error
stage_read_error:
        push    af
        ld      de,msg_stage_read
        call    print_text
        pop     af
        call    print_hex_a
terminate_error:
        xor     b
        ld      c,TERM0
        jp      BDOS

msg_usage:
        defm    "KSPPLAY.COM FILE.KSP [SONG] [SCC_SLOT_DECIMAL]$"
msg_01:
        defm    13,10,"01 PARSED"
msg_01_end:
msg_02a:
        defm    13,10,"02A BEFORE EXTBIO"
msg_02a_end:
msg_02b:
        defm    13,10,"02B EXTBIO OK"
msg_02b_end:
msg_02c:
        defm    " FREE=$"
msg_02:
        defm    13,10,"02 MAPPER$"
msg_03:
        defm    13,10,"03 OPEN$"
msg_04:
        defm    13,10,"04 SIZE$"
msg_04a:
        defm    " 04A MAGIC=$"
msg_04b:
        defm    " 04B VALIDATING$"
msg_04c:
        defm    " FIELDS 0C=$"
msg_04d:
        defm    " 0D=$"
msg_04e:
        defm    " LOAD=$"
msg_05:
        defm    13,10,"05 HEADER$"
msg_05a:
        defm    " STAGECOUNT=$"
msg_alloc:
        defm    13,10,"ALLOC$"
msg_alloc_stage:
        defm    " S=$"
msg_alloc_kss:
        defm    " K=$"
msg_alloc_main:
        defm    " M=$"
msg_06:
        defm    13,10,"06 STAGING$"
msg_06a:
        defm    " 06A SEEK$"
msg_06b:
        defm    " OK$"
msg_06c:
        defm    " 06C READ$"
msg_06d:
        defm    " OK$"
msg_07:
        defm    13,10,"07 READY$"
msg_08:
        defm    13,10,"08 PLAYER$"
msg_format:
        defm    "Only generic KCPX/KCPZ files are supported$"
msg_large:
        defm    "KSS file is too large for mapper staging$"
msg_mapper:
        defm    "Not enough free primary mapper segments$"
msg_extbio:
        defm    "EXTBIO mapper query failed$"
msg_dos:
        defm    "MSX-DOS2 file error$"
msg_stage_seek:
        defm    "STAGE SEEK ERROR$"
msg_stage_read:
        defm    "STAGE READ ERROR$"
msg_loading:
        defm    13,10,"LOADING KSS $"
msg_dot:
        defm    ".$"
msg_loaded:
        defm    13,10,"KSS LOADED",13,10,"$"
msg_track:
        defm    13,10,"TRACK: $"
msg_crlf:
        defm    13,10,"$"

filename:       defs    64,0
header:         defs    0x30,0
hex_buffer:     defs    2,0
file_handle:    defb    0
song_number:    defb    0
scc_slot:       defb    SCC_SLOT_DEFAULT
file_size:      defs    3,0
stage_count:    defb    0
stage_alloc_count:defb  0
stage_index:    defb    0
sparse_mode:    defb    0
sparse_initialized:defb 0
sparse_cache_count:defb 0
sparse_cache_next:defb  0
sparse_cache_slot:defb  0
sparse_cache_tags:defs  3,0
sparse_protected_segment:defb 0xFF
sparse_logical_segment:defb 0
sparse_physical_segment:defb 0
sparse_first_segment:defb 0
sparse_source_offset:defs 3,0
sparse_end_offset:defs 3,0
sparse_packed_size:defw 0
declared_banks: defb    0
bank_end:       defb    0
kcp_file:       defb    0
input_format:   defb    0
mapper_calls:   defw    0
mapper_free:    defb    0
mapper_total:   defb    0
stage_pool:     defs    4,0
stage_segments: defs    32,0
kss_segments:   defs    32,0
main_segments:  defs    4,0
trailer:        defs    24,0
ksp1_magic:     defm    "KSP1"
info_magic:     defm    "INFO"
kdir_magic:     defm    "KDIR"
meta_magic:     defm    "META"
title_key:      defm    "title="
title_entries_left: defb 0
title_skip_left: defb   0
title_length:   defb    0
title_delimiter:defb    0
title_byte:     defb    0
title_buffer:   defs    72,0
title_scratch:  defs    32,0

player_boot_blob:
        incbin  'KSPDOS2_PLAYER.raw'
player_boot_blob_end:
