; Default layout for assembling the bootstrap player.
; tools/build_msx_dos2_kss.py supplies an alternate include for the
; page-1-resident Quarth runtime build.

        org     0xC000

PLAYER_BASE:        equ 0xC000
PLAYER_RUNTIME_BASE: equ 0x52E4
PLAYER_RUNTIME_CONFIG_TARGET: equ 0x5C47
PLAYER_RUNTIME_ENTRY: equ 0x52F7

BIOS_ENASLT:    equ     0x0024
BIOS_CHPUT:     equ     0x00A2
BIOS_RAMAD1:    equ     0xF342
EXTBIO:         equ     0xFFCA
H_TIMI:         equ     0xFD9F

MAPPER_PAGE1:   equ     0xFD
MAPPER_PAGE2:   equ     0xFE
; Default OpenMSX/extb SCC slot ID: non-expanded primary slot 2.  The DOS2
; loader can override this with the optional fourth command argument for
; hardware whose SCC is in an expanded slot.
SCC_SLOT_DEFAULT: equ 0x02
SCC_SLOT_MAGIC:   equ 0xA5

RUNTIME_CONFIG:       equ 0xC960
RUNTIME_STAGE_TABLE:  equ RUNTIME_CONFIG
RUNTIME_MAIN_TABLE:   equ RUNTIME_STAGE_TABLE+32
RUNTIME_BASIC_P1:     equ RUNTIME_MAIN_TABLE+4
RUNTIME_BASIC_P2:     equ RUNTIME_BASIC_P1+1
RUNTIME_KSS_TABLE:    equ RUNTIME_BASIC_P2+1
RUNTIME_ORIGINAL_P2:  equ RUNTIME_KSS_TABLE+32
RUNTIME_PUT_P0:       equ RUNTIME_ORIGINAL_P2+1
RUNTIME_PUT_P1:       equ RUNTIME_PUT_P0+2
RUNTIME_PUT_P2:       equ RUNTIME_PUT_P1+2
RUNTIME_STACK_TOP:    equ RUNTIME_PUT_P2+2
RUNTIME_CONFIG_END:   equ RUNTIME_STACK_TOP+2
RUNTIME_BLOB_SOURCE:  equ RUNTIME_CONFIG_END
RUNTIME_BLOB_SIZE:    equ RUNTIME_BLOB_SOURCE+2
STAGE_BANK:           equ 12

BASIC_PTR:      equ     0x7FF0
BASIC_SIZE:     equ     0x7FF3
BASIC_STATE:    equ     0x7FF6
BASIC_SONG:     equ     0x7FF7
FIELD_SCRATCH:  equ     0xC800
; Complete-page materialization runs from the enlarged C000H bootstrap,
; which now ends just below D400H. Keep its transfer buffer at D400H-D7FFH
; and all copied page-3 runtime helpers above that buffer.
; Fixed page-3 TPA scratch area. Keep this after the complete bootstrap code
; and well below the runtime marker/stack reservation.
QCPX_SCRATCH:      equ  0xD600
QCPX_SCRATCH_SIZE: equ  0x0200

STACK_TOP:      equ     0xDA00
RETURN_STUB:    equ     0xD920
; Keep the live wrapper outside the retained QCPX parser/materializer code.
; Leave enough room for the QCPX return loop to save every engine register
; while it polls the keyboard.  Track switching abandons those saved values
; by resetting SP in the INIT trampoline; an ordinary tick restores them.
PLAY_WRAPPER:   equ     0xD950
PLAY_TARGET:    equ     0xCC9A
KSS_TABLE_SAVED: equ    0xCA00
SCC_SHADOW:     equ     0xCA20
SCC_PLUS_SHADOW: equ    0xCB20
KSS_STORAGE_SAVED: equ  0xCC9C
RAM_SLOT_SAVED: equ     0xCC9D
PAGE2_RESTORE_SAVED: equ 0xCC9E
PAGE1_RESTORE_SAVED: equ 0xCC9F
BANK0_HANDLER:  equ     0xCD00
BANK1_HANDLER:  equ     0xCE00
CUSTOM_ENASLT:  equ     0xD840
PUT_P0_DISPATCH: equ    0xD800
PUT_P1_DISPATCH: equ    0xD810
PUT_P2_DISPATCH: equ    0xD820
RST28_SAVED:    equ     0xD880
RST28_INSTALLED: equ   0xD884
SLOT_A8_SAVED:  equ     0xD885
SLOT_FFFF_SAVED: equ    0xD886
HTIMI_SAVED:    equ     0xD888
HTIMI_INSTALLED: equ   0xD88D
; Keep the bootstrap SCC handoff above all copied runtime helpers and below
; the QCPX scratch buffer. CC9FH used to overlap the enlarged QCPX parser.
RUNTIME_MARKER: equ     0xD9F0
; These two bytes live in the reserved tail, not in the player code.  The
; DOS2 front-end fills them before entering the bootstrap; Disk BASIC falls
; back to SCC_SLOT_DEFAULT when the magic byte is absent.
RUNTIME_SCC_SLOT:       equ RUNTIME_MARKER
RUNTIME_SCC_SLOT_VALID: equ RUNTIME_MARKER+1
PLAYER_RUNTIME_SCC_SLOT_TARGET:       equ 0x6290
PLAYER_RUNTIME_SCC_SLOT_VALID_TARGET: equ 0x6291
