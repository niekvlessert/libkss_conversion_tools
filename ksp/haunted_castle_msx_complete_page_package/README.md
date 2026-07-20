# Haunted Castle MSX-DOS2 complete-page KSP

This package contains an MSX-targeted materialization of the Haunted Castle
sound driver. It uses the repository's existing uncompressed `KCPX` complete-
page format.

## Runtime memory layout

| CPU region | Role |
|---|---|
| Page 0, `0000H-3FFFH` | Existing BIOS/MSX-DOS2 mappings |
| Page 1, `4000H-7FFFH` | One materialized 16 KiB engine/song image |
| Page 2, `8000H-BFFFH` | Real SCC cartridge |
| Page 3, `C000H-FFFFH` | Existing DOS2 resident area and KSP player/runtime |
| I/O `C4H-C7H` | MoonSound FM block |

The page-1 image is rebuilt for the selected command from one common template
and a small overlay. All 21 commands fit in one 16 KiB page.

## Page-1 layout

| Address range | Content |
|---|---|
| `4000H-57AFH` | Relocated driver and shared engine tables |
| `57B0H-5953H` | Relocated 21 x 10 command/channel pointer matrix |
| `5954H` | Selected command index |
| `5960H-5A98H` | KSP INIT/PLAY/STOP and hardware gateways |
| `5AA0H-5DB7H` | Relocated Haunted Castle work RAM |
| `5DC0H-6EFFH` | Per-song reachable sequences and resources |
| `6F00H-7FFBH` | Shared high-ROM resource tail |

The tightest command retains 305 bytes of unused dynamic page space.

## Hardware status

- K051649 accesses are retained for a real SCC in page 2.
- YM3812 register writes go directly to MoonSound FM ports `C4H/C5H`.
- INIT configures the YMF278B through `C6H/C7H` for OPL2-compatible FM mode.
- K007232 accesses currently use the virtual `D0H/D1H` gateway. The MSX player
  still needs an OPL4 PCM translator/sample uploader for that path.

## Rebuild

The source arcade ROM is not included. Supply the supported `768e01.e4` whose
SHA-1 is `c55f468c0da6afdaa2af65a111583c0c42868bd1`.

Run from this directory:

```sh
python3 build_haunted_castle_msx_complete_pages.py \
  --audio-rom /path/to/768e01.e4 \
  --full-builder build_haunted_castle_full_ksp_v2.py \
  --disassembly haunted_castle_recursive_disassembly.txt \
  --output rebuilt.ksp \
  --pages-dir rebuilt-pages \
  --report rebuilt-validation.json
```

Expected KSP:

- size: 63,487 bytes
- SHA-1: `3161ec5f386c4853c4120c58c899ff08183356f8`

## Validation performed

- all absolute operands in identified engine instructions were relocated;
- known indirect code-pointer tables were relocated;
- per-command sequence jumps and resource pointers were traced and relocated;
- all 21 page images were reconstructed from the KCPX template and overlays
  and compared byte-for-byte with the generated raw page images;
- an independent packaged rebuild reproduces the same KSP byte-for-byte.

Audio output has not yet been validated in openMSX or on real hardware. The
next test should use the repository's generic complete-page MSX-DOS2 path with
both SCC and MoonSound present.
