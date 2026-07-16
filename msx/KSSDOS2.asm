; MSX-DOS2 raw KSS/KSSX player loader.
;
; The build script appends a temporary C000H bootstrap and a second
; page-1-resident Quarth runtime blob.
; This COM program opens the user-supplied file through MSX-DOS2, allocates
; primary mapper segments with ALL_SEG, reads the raw file into allocated
; staging segments, and then transfers control to the resident player. The
; player. For Quarth the bootstrap copies the second blob to the free tail
; of the engine's page-1 image before playback starts; other engines retain
; the legacy page-3 runtime path.
;
; Usage: KSSPLAY.COM FILE.KSS [SONG]

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
PLAYER_RUNTIME_BLOB_SOURCE: equ 0xC9AF
PLAYER_RUNTIME_BLOB_SIZE:   equ 0xC9B1
PLAYER_RUNTIME_BASE:        equ 0x52E4
PLAYER_RUNTIME_CONFIG_TARGET: equ 0x5C47
PLAYER_RUNTIME_ENTRY:       equ 0x52F7
; These bytes are in the unused fixed page-3 tail of the bootstrap.  The
; optional fourth command-line argument selects the SCC slot ID (decimal).
PLAYER_RUNTIME_SCC_SLOT:       equ 0xD2F0
PLAYER_RUNTIME_SCC_SLOT_VALID: equ 0xD2F1
PLAYER_RUNTIME_SCC_SLOT_TARGET:       equ 0x6232
PLAYER_RUNTIME_SCC_SLOT_VALID_TARGET: equ 0x6233
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
        ; Read five bytes beyond the fixed KSSX header as a cheap QCPX
        ; signature probe.  The complete-page format starts with its one-byte
        ; load image at 20H and "QCPX" at 21H.
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
        ld      a,(stage_segments)
        call    print_hex_a
        ld      a,(stage_segments+1)
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

        ld      a,(file_handle)
        ld      b,a
        ld      c,CLOSE
        call    BDOS
        jp      nz,dos_error

        ; QCPX is already a complete 16K-page container. Never pass it
        ; through the legacy F-1 Spirit 8K selector patcher.
        ld      a,(qcpx_file)
        or      a
        jr      nz,patch_file_done
        call    patch_f1_if_needed
        jp      c,format_error
patch_file_done:
        ld      de,msg_07
        call    print_text
        ld      de,msg_loaded
        call    print_text

        ; No DOS calls are made after this point.  Hand the existing player
        ; its normal BASIC-compatible state and let it take over C000H.
        ld      de,msg_08
        call    print_text
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

        ; Install the dynamic mapper layout in the bootstrap config. The
        ; Quarth bootstrap later copies this block into the page-1 runtime.
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
        ld      hl,player_runtime_blob
        ld      (PLAYER_RUNTIME_BLOB_SOURCE),hl
        ld      hl,player_runtime_blob_end-player_runtime_blob
        ld      (PLAYER_RUNTIME_BLOB_SIZE),hl
        jp      0xC000

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
        ld      ix,stage_segments
        ld      a,(stage_count)
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
        ld      a,(header+0x0D)
        and     0x80
        jr      nz,inspect_8k
        ld      a,(header+0x0D)
        and     0x7F
        ld      (declared_banks),a
        cp      33
        jr      nc,inspect_bad_bank_count
        ld      a,(header+0x0C)
        ld      c,a
        ld      a,(declared_banks)
        add     a,c
        jr      c,inspect_bad
        ld      (bank_end),a
        call    probe_qcpx_header
        or      a
        ret
inspect_8k:
        ; The shipped F-1 Spirit KSCC is the known 8K exception.  Its two
        ; 8K banks can be represented as one 16K mapper segment after the
        ; two selector sequences are patched in the staged copy.
        ld      a,(header+0x0C)
        cp      14
        jr      nz,inspect_bad_8k_banks
        ld      a,(header+0x0D)
        and     0x7F
        cp      2
        jr      nz,inspect_bad_8k_mode
        ld      hl,(header+4)
        ld      a,h
        cp      0x5F
        jr      nz,inspect_bad_8k_load_hi
        ld      a,l
        or      a
        jr      nz,inspect_bad_8k_load_lo
        ld      a,1
        ld      (patch_f1),a
        xor     a
        ld      (declared_banks),a
        inc     a
        ld      (declared_banks),a
        ld      a,15
        ld      (bank_end),a
        or      a
        ret
inspect_bad:
        ld      a,1
        jr      inspect_bad_kind
inspect_bad_8k_banks:
        ld      a,2
        jr      inspect_bad_kind
inspect_bad_8k_mode:
        ld      a,3
        jr      inspect_bad_kind
inspect_bad_8k_load_hi:
        ld      a,4
        jr      inspect_bad_kind
inspect_bad_8k_load_lo:
        ld      a,5
        jr      inspect_bad_kind
inspect_bad_bank_count:
        ld      a,6
inspect_bad_kind:
        ld      (error_kind),a
        scf
        ret

probe_qcpx_header:
        xor     a
        ld      (qcpx_file),a
        ld      a,(header+2)
        cp      'S'
        ret     nz
        ld      a,(header+0x21)
        cp      'Q'
        ret     nz
        ld      a,(header+0x22)
        cp      'C'
        ret     nz
        ld      a,(header+0x23)
        cp      'P'
        ret     nz
        ld      a,(header+0x24)
        cp      'X'
        jr      z,probe_qcpx_header_found
        cp      'Z'
        ret     nz
probe_qcpx_header_found:
        ld      a,1
        ld      (qcpx_file),a
        ; The five logical pages are records in one container.  Native MSX
        ; playback materializes only the selected record, so one physical
        ; mapper segment is sufficient.
        ld      (declared_banks),a
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

stage_file:
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

patch_f1_if_needed:
        ld      a,(patch_f1)
        or      a
        ret     z
        ld      a,(stage_segments)
        call    mapper_put_p2
        ld      a,(0x800D)
        cp      0x82
        jr      nz,patch_f1_bad

        ; Keep the 8K mode byte intact.  Replace each three-byte
        ; LD (9000H),A / LD (B000H),A selector store with RST+NOP+NOP.
        ; The RST vectors are installed by the relocated player and return
        ; to the instruction following the original three-byte store.
        ld      hl,0x8013
        ld      de,f1_old_9000
        ld      bc,3
        call    patch_f1_site
        jr      nz,patch_f1_bad
        ld      hl,0x8018
        ld      de,f1_old_b000
        ld      bc,3
        call    patch_f1_b000_site
        jr      nz,patch_f1_bad
        ld      hl,0x8093
        ld      de,f1_old_9000
        ld      bc,3
        call    patch_f1_site
        jr      nz,patch_f1_bad
        ld      hl,0x8098
        ld      de,f1_old_b000
        ld      bc,3
        call    patch_f1_b000_site
        jr      nz,patch_f1_bad
        ld      hl,0x8EB5
        ld      de,f1_old_9000
        ld      bc,3
        call    patch_f1_site
        jr      nz,patch_f1_bad
        ld      hl,0x8EBC
        ld      de,f1_old_9000
        ld      bc,3
        call    patch_f1_site
        jr      nz,patch_f1_bad
        ld      hl,0x8F16
        ld      de,f1_old_9000
        ld      bc,3
        call    patch_f1_site
        jr      nz,patch_f1_bad
        ld      hl,0x8F2F
        ld      de,f1_old_9000
        ld      bc,3
        call    patch_f1_site
        jr      nz,patch_f1_bad
        or      a
        ret
patch_f1_bad:
        scf
        ret

patch_f1_site:
        push    hl
        call    compare_bytes
        pop     de
        ret     nz
        ld      hl,f1_new_rst
        ld      bc,3
        ldir
        xor     a
        ret

patch_f1_b000_site:
        push    hl
        call    compare_bytes
        pop     de
        ret     nz
        ld      hl,f1_new_rst_b000
        ld      bc,3
        ldir
        xor     a
        ret

compare_bytes:
        ld      a,(hl)
        push    hl
        ex      de,hl
        cp      (hl)
        ex      de,hl
        pop     hl
        ret     nz
        inc     hl
        inc     de
        dec     bc
        ld      a,b
        or      c
        jr      nz,compare_bytes
        xor     a
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

usage_error:
        ld      de,msg_usage
        jr      print_and_exit
format_error:
        ld      a,(error_kind)
        cp      1
        jr      z,format_bad_magic
        cp      2
        jr      z,format_bad_8k_banks
        cp      3
        jr      z,format_bad_8k_mode
        cp      4
        jr      z,format_bad_8k_load_hi
        cp      5
        jr      z,format_bad_8k_load_lo
        cp      6
        jr      z,format_bad_bank_count
        ld      de,msg_format
        jr      print_and_exit
format_bad_magic:
        ld      de,msg_bad_magic
        jr      print_and_exit
format_bad_8k_banks:
        ld      de,msg_bad_8k_banks
        jr      print_and_exit
format_bad_8k_mode:
        ld      de,msg_bad_8k_mode
        jr      print_and_exit
format_bad_8k_load_hi:
        ld      de,msg_bad_8k_load_hi
        jr      print_and_exit
format_bad_8k_load_lo:
        ld      de,msg_bad_8k_load_lo
        jr      print_and_exit
format_bad_bank_count:
        ld      de,msg_bad_bank_count
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
        defm    "KSSPLAY.COM FILE.KSS [SONG] [SCC_SLOT_DECIMAL]$"
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
        defm    13,10,"07 PATCH$"
msg_08:
        defm    13,10,"08 PLAYER$"
msg_format:
        defm    "Unsupported or invalid KSS file$"
msg_bad_magic:
        defm    "KSS header magic check failed$"
msg_bad_8k_banks:
        defm    "F1 header bank count is not 14$"
msg_bad_8k_mode:
        defm    "F1 header mode is not 82H$"
msg_bad_8k_load_hi:
        defm    "F1 load address high byte is not 5FH$"
msg_bad_8k_load_lo:
        defm    "F1 load address low byte is not zero$"
msg_bad_bank_count:
        defm    "KSS declares more than 32 banks$"
msg_large:
        defm    "KSS file is too large for mapper staging$"
msg_mapper:
        defm    "Not enough contiguous primary mapper segments$"
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

f1_old_9000:
        defb    0x32,0x00,0x90
f1_old_b000:
        defb    0x32,0x00,0xB0
f1_new_rst:
        defb    0xF7,0x00,0x00
f1_new_rst_b000:
        defb    0xEF,0x00,0x00
filename:       defs    64,0
header:         defs    0x30,0
hex_buffer:     defs    2,0
file_handle:    defb    0
song_number:    defb    0
scc_slot:       defb    SCC_SLOT_DEFAULT
file_size:      defs    3,0
stage_count:    defb    0
stage_index:    defb    0
declared_banks: defb    0
bank_end:       defb    0
error_kind:     defb    0
patch_f1:       defb    0
qcpx_file:      defb    0
mapper_calls:   defw    0
mapper_free:    defb    0
mapper_total:   defb    0
stage_segments: defs    32,0
kss_segments:   defs    32,0
main_segments:  defs    4,0

player_boot_blob:
        incbin  'KSSDOS2_PLAYER.raw'
player_boot_blob_end:
player_runtime_blob:
        incbin  'KSSDOS2_RUNTIME.raw'
player_runtime_blob_end:
