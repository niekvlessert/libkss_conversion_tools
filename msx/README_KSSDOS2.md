# MSX-DOS2 KSS player

`KSSPLAY.COM` reads an unmodified `.KSS` or `.KSSX` file through MSX-DOS2
file handles and stages it in primary RAM-mapper segments. It does not convert,
wrap, or rewrite the KSS file.

Run it from an MSX-DOS2 prompt:

```text
KSSPLAY HAPPY.KSS 0
```

The first argument is the KSS filename and the optional second argument is the
song number, defaulting to song 0. Put `KSSPLAY.COM` and the KSS file in the
same mounted disk directory, or provide a DOS2 path.

For the included F-1 Spirit file, track 54 can be started with:

```text
KSSPLAY F1.KSS 54
```

The loader prints `LOADING KSS` followed by one dot per 16 KiB read, then
prints `KSS LOADED`. The input file is never modified. The verified F-1 Spirit
8K-bank layout is adapted in the staged RAM copy; other generic 8K-bank KSS
files are still rejected.

The loader keeps the player and its mapper handlers in the fixed page-3 TPA
area. It allocates only staging segments and logical KSS data segments with
`ALL_SEG`; page 3 is never mapped to an allocated player segment. The resident
area is below the DOS-resident boundary and outside the runtime stack. A
512 KiB primary mapper is recommended for the included banked test files.

The player supports ordinary KSCC/KSSX files and uncompressed 16K banked data
whose declared bank range does not overlap the player/main/staging ranges. The
sound hardware named by the KSS header must be present; insert SCC or
MoonSound/OPL4 as required by the selected file.

Build or rebuild the COM with:

```sh
python3 tools/build_msx_dos2_kss.py
```

The COM uses MSX-DOS2 function calls 43H/48H/4AH/45H for open/read/seek/close
and the DOS2 extended-BIOS mapper support routines for primary-segment
allocation and page-2 mapping.

### Logical mapper contract

The MSX player owns physical mapper placement. It allocates staging and KSS
data segments at runtime, stores their physical segment numbers in a
logical-to-physical table, and translates the engine's bank requests before
changing the page-2 mapper. A relocated engine must expose:

ENGINE_INIT
ENGINE_PLAY
ENGINE_STOP
ENGINE_MAP_DATA

ENGINE_MAP_DATA is a logical request. The engine supplies the logical bank
number in A and enters through the player-installed user RST 28H gateway; it
must not write a physical segment number directly to mapper port FEH. The
player saves and patches the four bytes at 0028H, then restores them on stop.
The gateway and its handlers are in fixed page-3 TPA memory, so neither the
patched RST instruction nor its return address is in page 2.

The loader obtains the original page-2 segment with DOS2 `GET_P2` before any
playback setup. Every runtime mapper change uses the corresponding DOS2
`PUT_P0`, `PUT_P1`, or `PUT_P2` entrypoint. `PUT_P2` changes the mapper
segment and DOS2 shadow state, but it does not select the mapper RAM slot;
the player selects the RAM slot separately before calling it.

The uncompressed Quarth test image uses this contract. Rebuild the COM and
copy it and the raw image to the MSX-DOS2 disk, then run:

    python3 tools/build_msx_dos2_kss.py
    KSSPLAY.COM QUARTH.KSS 0
    KSSPLAY.COM QUARTH.KSS 15

The two commands exercise Quarth's bank-0 and bank-1 songs while the physical
segments are chosen by the loader. The current generic COM materializes raw
KSS/KSSX bank data; the private QRTX compression used by
quarth_16kb_max_compressed.kss is supported by the desktop libkss player but
still needs a corresponding MSX decompression/materialization path.

### Important native-player limits

The physical segment numbers are not KSS bank numbers. ALL_SEG allocates
whatever physical segments DOS2 can provide; the loader records those IDs in
the logical-to-physical table. Runtime page-1/page-2 changes go through the
DOS2 PUT_P1/PUT_P2 entrypoints passed into the resident player. A trace
therefore shows the physical mapper write inside the DOS2 mapper routine, not
in the Quarth engine or its logical bank handler.

This is a specialized native adapter, not a generic DOS2 KSS executor. It
does not relocate arbitrary engines safely, does not support general 8K KSS
windows, and does not support the private QRTX compressed Quarth format.
The QCPX Quarth path keeps its engine, selected tracks, and relocated writable
work area together in mapper-RAM page 1. Page 2 remains selected to the real
SCC cartridge for INIT and PLAY. The player selects the configured SCC slot
and enables the register window with a `3FH` write to `9000H`; the adapted
engine no longer performs its original `FFFDH`/`FFFEH` slot-shadow writes.
The bootstrap code ends below `D400H`; `D400H..D7FFH` is the materialization
scratch buffer, fixed mapper/slot helpers start at `D800H`, and the SCC
handoff is stored at `D9F0H/D9F1H`.

While QCPX is playing, use Cursor Left/Right to select the previous/next
contiguous KSS song ID, Space to restart, and Escape or Ctrl-C to return to
DOS2. Selection wraps
at IDs 0 and 18. A change silences the chips, rebuilds the requested complete
page in the same allocated page-1 mapper segment, runs Quarth INIT, and resumes
the interrupt-timed PLAY loop. This also works when the next song belongs to a
different QCPX logical page; no second runtime mapper page is required.

Keyboard polling reads PPI row 8 directly. It deliberately avoids BIOS
`CHSNS`/`CHGET`: those calls can enter slot-dependent BIOS code while page 1
contains Quarth rather than the normal system mapping. The poll runs with
interrupts disabled and preserves AF, BC, DE, HL, IX, and IY, because Quarth
retains useful register state between PLAY calls. The resident loop is at
`D920H`, its direct PLAY wrapper is at `D950H`, and the complete-page
parser/materializer remains resident in fixed page-3 TPA memory.

The compressed `QCPZ` variant uses the same controls and complete-page
addresses. Build it and the DOS2 player with:

```sh
python3 tools/build_quarth_16k_complete_page_compressed.py
python3 tools/build_msx_dos2_kss.py
```

Copy `vigamup/extracted/quarth_16k_complete_page_compressed.kss` as, for
example, `QCPZ.KSS`, then run `KSSPLAY.COM QCPZ.KSS 0`. The engine and common
block are ZX0-expanded only at startup. A same-logical-page track change only
clears the relocated workspace and calls INIT. A cross-page change temporarily
replaces SCC page 2 with the staged compressed stream, expands only the new
music payload into the existing page-1 segment, applies its pointer patches,
then restores and enables the real SCC.

The staging stream is selected with direct `$A8/$FFFF` bit masks. Calling BIOS
`ENASLT` here can lose the fixed page-3 TPA mapping on an expanded RAM slot.
The direct routine changes only page 2's primary/secondary bits and preserves
page 3 exactly.

### Salamander SCPX/SCPZ

Build and stage both Salamander complete-page variants with:

```sh
python3 tools/build_salamander_complete_page.py
python3 tools/build_salamander_complete_page_compressed.py
python3 tools/build_msx_dos2_kss.py
cp vigamup/extracted/salamander_complete_page.kss msx/SCPX.KSS
cp vigamup/extracted/salamander_complete_page_compressed.kss msx/SCPZ.KSS
```

Then start contiguous track 5 (original Salamander ID 30) with:

```text
KSSPLAY.COM SCPZ.KSS 5
```

Cursor Left/Right changes tracks, Space restarts the current track, and Escape
or Ctrl-C returns to COMMAND2.COM. Salamander
uses the same physical layout as QCPX/QCPZ: engine plus selected music in one
writable page-1 segment, real SCC in page 2, and fixed player/DOS2 state in
page 3. Its original direct BIOS `WRTPSG` calls at `0093H` are replaced by a
resident direct-port gateway because DOS2 page 0 is not guaranteed to expose
the BIOS slot.

The four-item interface is the target contract for adapted engines. The
current KSSPLAY.COM uses fixed page-3 TPA residency. For complete-page files,
Escape/Ctrl-C silences the chips, restores mapper RAM and the original page-1
and page-2 segments, restores any owned vectors, and terminates through DOS2.
DOS2 then reclaims the mapper segments allocated by the process.

### Generic KCPX/KCPZ status

The desktop libkss changes in this repository recognize generic KCPX/KCPZ and
materialize the selected 16K template plus sparse song overlay before INIT.
The native `KSSPLAY.COM` currently recognizes the specialized QCPX/QCPZ and
SCPX/SCPZ variants only. Generic files such as
`nemesis2_complete_page_compressed.kss` therefore require a native parser for
the KCP fixed header, logical-to-original song map, common template, and sparse
overlay records before they can be called MSX-compatible. The raw KCPX overlay
can be applied in place to the allocated page-1 segment. KCPZ additionally
requires streaming ZX0 expansion or temporary staging; its complete sparse
overlay is not guaranteed to fit the existing 1 KiB page-3 scratch area.
