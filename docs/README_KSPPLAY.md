# MSX-DOS2 KSP player

`KSPPLAY.COM` is the native MSX-DOS2 player for this repository's extended
KSP files. It supports two payload families already supported by the host
`kspplayer`:

- generic Konami complete pages with internal `KCPX` or `KCPZ` markers;
- compact `KSP1` resource files containing a MoonSound `ENGN` and selected
  `SONG` chunk. The native route currently supports MBWave engine type 1
  without an embedded MWK wavebank.

Raw stock KSS/KSSX, historical game-specific QCP/SCP containers and arbitrary
renamed files are intentionally unsupported.

```text
KSPPLAY QUARTH.KSP 0
KSPPLAY ALMOSEND.KSP 0
```

The optional second argument is the song ID and defaults to zero. Cursor
Left/Right changes songs, wrapping at both ends, and Escape or Ctrl-C silences
the chips and returns to DOS2. Space also restarts Konami songs. Live
MoonSound switching is supported for compact resource packs containing
multiple self-contained MWM `SONG` chunks that do not require an MWK wavebank.

The player prints the selected track title before playback. Konami titles
come from the existing INFO records; compact MoonSound titles come from the
KSP META chunk.

Build the COM with:

```sh
python3 tools/build_msx_dos2_ksp.py
```

## Konami memory layout

```text
Page 0  0000H-3FFFH  unchanged DOS2/BIOS environment
Page 1  4000H-7FFFH  selected engine + music + writable state
Page 2  8000H-BFFFH  real SCC cartridge during INIT and PLAY
Page 3  C000H-FFFFH  fixed player in free TPA + DOS2/system area
```

KCPZ temporarily exposes staged mapper RAM in page 2 while decompressing,
then restores and enables the SCC slot. Logical page numbers are translated
to physical segments allocated by DOS2; converted engines never contain
physical segment IDs.

Compressed streams are allowed to cross a 16K staged-file boundary. The ZX0
input routine switches to the next independently allocated mapper segment at
`C000H`; the segments do not need to be physically contiguous.

When the complete file plus the two KCPZ work segments does not fit in the
mapper space left free by DOS2, the loader uses sparse staging. It retains
file segment 0 (header, maps and common template) and only the one or two
segments containing the selected track overlay. Five free 16K segments are
the minimum: three staging segments, one materialized page and one overlay
temporary. If six are free, the fourth staging segment becomes a third music
cache line. File segment 0 remains pinned, while the remaining two or three
lines retain every compressed track overlay that fits. Cursor Left/Right only
reads the floppy on a cache miss and then restarts playback. A 256K FS-A1ST
configuration with six free DOS2 mapper segments therefore plays the 118K
SD Snatcher pack and usually changes several adjacent tracks without I/O.

## MoonSound memory layout

Compact KSP1 files are parsed through their trailer and KDIR directory. ENGN
is materialized in page 1 and SONG in page 2. Raw MWM pattern-block headers
are compacted in place exactly like the host materializer. Uncompressed SONG
chunks may cross staged-file segment boundaries.

MBWave requires writable state at `DA00H-DAFFH`, which overlaps the DOS2
resident boundary on the Panasonic FS-A1ST. During playback the player
therefore uses three allocated segments:

```text
Page 1  engine
Page 2  compacted song + resident loop at B800H, stack below B700H
Page 3  temporary MBWave work RAM (DA00H state)
```

The B800H loop keeps interrupts disabled and uses VDP status-register 0 as a
CPU-speed-independent 60 Hz clock, so playback has the same tempo in Z80 and
R800 modes. It scans Escape and the cursor keys without calling DOS while the
temporary page-3 work segment is visible. Escape silences OPL4, restores the
original page-3 segment, jumps back into the fixed DOS2 player, restores page
1/page 2 and terminates.

For Cursor Left/Right the loop first silences OPL4 and restores the original
page 3, then reparses and rematerializes the selected `SONG` from the KSP data
already staged in mapper RAM. The shared `ENGN` is installed again and
playback restarts without reopening the file or reading the floppy.

## Canonical files

The `msx/` directory contains `KSPPLAY.COM`, the MoonSound tests
`ALMOSEND.KSP` and `INTRO1.KSP`, and generic Konami files:
`QUARTH.KSP`, `SALAMAND.KSP`, `F1SPIRIT.KSP`, `NEMESIS2.KSP`,
`NEMESIS3.KSP`, `PARODIUS.KSP`, `KINGVAL2.KSP`, `CONTRA.KSP`,
`SPACEMAN.KSP`, `SOLIDSNK.KSP`, and `SDSNATC.KSP`.

The outer 32-byte prefix remains KSSX-compatible because the host player and
materializers use it for load/init/play metadata. The `.KSP` extension names
the extended container contract; stock libkss support is not implied.
