# Konami KSS conversion playbook

This is the handoff document for separating Konami music engines from their
music data and rebuilding the result as banked KSS. It records the Quarth
engine analysis and the workflow that should be reused for F-1 Spirit,
Nemesis, and the other Konami packs.

## Why a raw banked KSS can be larger

The standard KSCC header is only 16 bytes. The important storage rule is that
raw bank data is not variable-sized: every declared 16K bank occupies exactly
`0x4000` bytes after the initial load image. Byte `0x0D` contains the bank
count; its high bit selects 8K mode, and a clear high bit selects 16K mode.

Quarth demonstrates the size tradeoff:

| image | initial load | bank payload | header | total |
|---|---:|---:|---:|---:|
| original `quarth.kss` | `0x7824` = 30,756 | 0 | 16 | 30,772 |
| repacked `quarth_16k.kss` | `0x12E3` = 4,835 | `2 * 0x4000` = 32,768 | 16 | 37,619 |

The repacked file is therefore 6,847 bytes larger. That difference is not an
unexplained KSS trailer:

- The shared Quarth lookup block is `0x12D1` = 4,817 bytes and must exist in
  both physical banks, because either bank can be mapped at `8000H` while the
  driver is running.
- Bank 0 uses `0x3FFA` = 16,378 bytes: the common block, the streams for
  tracks 3–14, and the small `A0F0H` volume-data dependency. It has 6 padding
  bytes.
- Bank 1 uses `0x316D` = 12,653 bytes: the common block and the streams for
  tracks 15–21. It has 3,731 padding bytes.
- Tracks 14 and 15 share a stream start at `906DH`; each self-contained bank
  retains its own copy of the overlapping stream range.

The two banks are necessary for the selected Quarth tracks. A single 16K bank
cannot hold the common block plus the complete stream ranges for tracks 3–21.
The original image has no such rounding cost because its engine and music are
one contiguous 30,756-byte load image.

The raw result can be made smaller later with a KSSX/container-specific
compression layer. That layer can compress the common block once and omit
padding, then materialize normal 16K banks before the engine starts. The
existing `build_nemesis3_16kb_max_compressed.py` does this for Nemesis 3 with a
custom player-side expansion format. It is a separate optimization from the
correctness-critical raw bank layout.

## Quarth engine map

The original Quarth KSCC image is:

```text
load image: 4000H..B824H
driver core: 4000H..52A4H
music/common data: 52A4H..B800H
tail/helper: B800H..B824H
init: B817H
play: B820H
```

The exported `vigamup/extracted/quarth.engine` is a concatenation of the
driver core and the tail. It is not one naturally contiguous address range:
the first `0x12A4` bytes belong at `4000H`, and the final `0x24` bytes belong
at `B800H`. Disassemble those fragments with their real origins when using
`z80dasm`.

The static code/data scan shows that this is genuinely a music player, not a
large game subsystem hidden inside the 31 KB load image:

- the executable core is `4000H..52A4H`, 4,772 bytes;
- the extracted tail is 36 bytes, including the original entry wrappers;
- the core contains only small support tables, such as the command dispatch
  table at `4B75H`, a 13-byte note table at `44EBH`, and a 12-byte output
  lookup at `4FD0H`;
- runtime state is external, mainly eight 64-byte channel blocks at
  `C400H..C5FFH` and control state at `C600H..C68BH`;
- the core's remaining external accesses are chip/slot handling at `9880H`,
  `FFFDH`, and `FFFEH`, not game logic.

There is therefore no meaningful bulk of removable Quarth game overhead. The
large original image is music descriptors, lookup data, and event streams.
The complete-page build adds only the required 64-byte compatibility tail to
the 4,772-byte core; its 4,836-byte shared engine template is still almost
entirely the original player.

The original entry stubs are:

```text
4000H -> driver reset/init setup
4003H -> song selection
4006H -> play tick
```

The repacker keeps the core at `4000H`, moves the helper next to it, and
installs a small wrapper:

```text
main image: 4000H..52E3H
relocated helper: 52A4H
new init: 52BBH
new play: 52DFH
bank window: 8000H..BFFFH, selected with OUT (FEH),A
```

The wrapper selects bank 0 before the original reset code runs, calls the
original init path with the original song number in `A`, selects the song
bank, and then calls the original selector. Tracks 3–14 use bank 0; tracks
15–21 use bank 1.

## Song selection and descriptors

Quarth does not have a 256-entry pointer table. The selector indexes a compact
word table at `52A2H` as:

```text
descriptor = word(52A2H + 2 * song_id)
```

For the real music set, IDs 3–21 point into the compact descriptor area.
Other entries in this region are internal sound-effect/control descriptors.
Treating the area as 256 pointers corrupts descriptors and lookup data.

Each descriptor begins with a two-byte header consumed by the selector,
followed by a variable number of little-endian stream pointers. The descriptor
ends where the next descriptor in the compact table begins; it is not always
an eight-pointer record. In the selected set, descriptors contain either six
or eight stream pointers.

The engine creates eight channel work areas at:

```text
C400H, C440H, C480H, C4C0H,
C500H, C540H, C580H, C5C0H
```

The stream pointer for a channel is at `IX+02/03` when that channel is active.
The current and target tone periods are maintained around `IX+0C/0D` and
`IX+11/12`; timing, effect, and mode state occupy the rest of the 40-byte
channel structure.

## Tone and effect data

Quarth is an event-stream engine, not a PCM player. Stream bytes below `D0H`
are interpreted as note/timing data. Bytes `D0H` and above dispatch through a
command pointer table at `4B75H`.

Important relocated tables include:

```text
5482H, 549AH, 54B2H, 54CAH   four 12-entry effect/voice pointer tables
5838H                         90-entry frequency lookup pointer table
64C5H                         sentinel plus 12 pitch/effect pointers
642AH                         eight-entry volume/envelope pointer table
```

The entries in the four `54xxH` tables are themselves pointers. Relocating
only the executable `LD HL,54xxH` operands is insufficient. The same applies
to the frequency, pitch, and volume pointer tables.

The volume table's first pointer is special: it points at `A0F0H`, which is
music-side data rather than part of the common lookup block. The Quarth
repacker copies that dependency into bank 0 and relocates the table entry.

Several high command bytes carry absolute stream addresses. In particular,
`F9H`, `FCH`, and `FDH` contain little-endian control-flow targets. `FDH` is
the direct stream jump seen in track 5 as `FD 93 67`; after relocation that
target must point into the banked copy, not back to `6793H`. `F9H` and `FCH`
are loop/repeat-related forms that also carry stream targets.

The PSG output path writes register/value pairs through ports `A0H` and `A1H`.
The game metadata identifies both PSG and SCC, and the driver also performs
SCC-side memory-mapped output. The first three tone periods follow the normal
PSG period registers; the remaining channel state and effect processing feed
the SCC/other output paths. This is why a valid conversion must compare both
the stream state and the generated chip writes, not merely check that the Z80
continues executing.

The recovered effect model includes:

- note periods with target-period updates for slides and pitch changes;
- per-channel timing counters and note lengths;
- mode/flag commands that switch between ordinary notes and effect streams;
- lookup-driven pitch and volume/envelope sequences;
- loop and jump commands embedded directly in the stream data.

## Current Quarth conversion

The implementation is [`tools/repack_quarth_banked.py`](../tools/repack_quarth_banked.py).
It currently uses:

```text
common copy:     52A2H..6573H -> 8000H..92D1H in each bank
bank 0 streams:  6573H..911CH plus A0F0H..A270H
bank 1 streams:  906DH..AF09H
required IDs:    vigamup/quarth.trackinfo only
```

It patches four classes of references:

1. executable absolute references to the compact table and lookup tables;
2. nested pointers inside common lookup tables;
3. variable-length descriptor stream pointers;
4. absolute `F9H/FCH/FDH` stream operands.

The resulting file is [`vigamup/extracted/quarth_16k.kss`](../vigamup/extracted/quarth_16k.kss).

## Quarth compressed output

The maximally compressed build is generated with
[`tools/build_quarth_16kb_max_compressed.py`](../tools/build_quarth_16kb_max_compressed.py):

```sh
python3 tools/build_quarth_16kb_max_compressed.py
```

It writes [`vigamup/extracted/quarth_16kb_max_compressed.kss`](../vigamup/extracted/quarth_16kb_max_compressed.kss).
The KSSX song IDs are now contiguous `0..18`, mapped in trackinfo order to:

```text
0..18 -> 5,3,9,10,11,4,12,13,14,6,15,16,17,18,19,20,21,7,8
```

The companion metadata is
[`vigamup/quarth_16kb_max_compressed.trackinfo`](../vigamup/quarth_16kb_max_compressed.trackinfo).
The private `QRTX` tail compresses the engine, one universal common block,
and two bank-specific data areas. The bundled libkss runtime reconstructs the
normal two physical 16K banks before playback; this requires the Quarth QRTX
support in `3rd_party/libkss/src/kssplay.c`, just as the existing compressed
Nemesis 3 output requires its `N3ZX` support.

The bootstrap itself consumes some Z80 cycles during initialization, so a
raw write-trace comparison should either exclude that startup interval or
compare after alignment. The music stream and bank selections remain stable;
all 19 remapped IDs load and select the expected bank in the short validation
run.

## Quarth complete-page container

For the stricter MSX layout, duplicating the engine in every mapper bank is
unnecessary. `tools/build_quarth_16k_complete_page.py` writes
`quarth_16k_complete_page.kss`, a private uncompressed `QCPX` KSSX
container. It stores one copy of the 4,836-byte engine-plus-wrapper template
and one 4,817-byte common lookup template. The five logical pages contain
only page-specific music payloads and descriptor patches:

```text
page 0: tracks 3,4,5,6,7,8       music 3885 bytes
page 1: tracks 9,10,11            music 4582 bytes
page 2: tracks 12,13,14           music 3687 bytes
page 3: tracks 15,16,17,18        music 4883 bytes
page 4: tracks 19,20,21           music 3721 bytes
```

The player materializes each page as:

```text
4000H..52E4H  engine, helper and fixed entry wrappers
52E4H..65B5H  relocated common tables/descriptors
65B5H..       page-specific streams and the A0F0H dependency
7D00H..7F8BH  writable channel/control state (relocated from C400H..C68BH)
```

The complete page is mapped into the `4000H..7FFFH` window, leaving
`8000H..BFFFH` available for the SCC cartridge. KSS song IDs are contiguous
`0..18` and map to the required original Quarth IDs in trackinfo order.
QCPX mapper pages must be writable: the engine's state now lives in their
reserved tail. The six original `FFFDH`/`FFFEH` to `OUT (A8H)` sequences are
removed because the native player owns slot selection and keeps page 2 on the
SCC for INIT and PLAY.

The temporary DOS2 bootstrap now extends beyond `CC9FH`. Handoff bytes and
fixed helper destinations must not overlap that code. The current bootstrap
ends below `D300H`, the transfer scratch buffer occupies `D300H..D6FFH`, the
mapper/slot dispatches begin at `D700H`, the live return loop and PLAY wrapper
are at `D820H` and `D850H`, and the SCC slot/magic handoff is at
`D8F0H/D8F1H`. Its former `CC9FH` location overwrote an instruction operand
and the next opcode during common-data materialization, which restarted the
COM entry after the first handoff.

The host `kssplayer` and libkss materializer support this container. The
native MSX-DOS2 loader materializes only the selected logical page into one
allocated physical mapper segment.

For live selection, the native player retains the QCPX parser and materializer
in its fixed page-3 TPA block. Cursor Left/Right changes the contiguous KSS ID,
Space restarts it, and the player rebuilds the requested complete page in the
same physical page-1 segment before calling INIT. This avoids both duplicated
engine pages in the file and a second live mapper page. The brief stop and
rematerialization happens only at song changes.

Do not poll the keyboard through BIOS `CHSNS` or `CHGET` in this layout. Page 1
then contains Quarth, so BIOS calls can follow slot-dependent paths that assume
the normal system mapping. Read PPI keyboard row 8 from fixed page-3 code,
prevent interrupts during the temporary row selection, debounce it, and save
all engine registers around the poll. The first implementation omitted that
register preservation and turned otherwise valid playback into noise.

### Compressed complete pages (QCPZ)

`tools/build_quarth_16k_complete_page_compressed.py` converts the proven QCPX
container into QCPZ. It keeps the song maps and relocation/patch records but
ZX0-compresses the engine, common block, and five page payloads separately.
The current result is 16,685 bytes instead of 32,265 bytes for QCPX. Every
compressed stream is placed wholly inside one absolute 16K file-staging
segment, allowing a normal forward ZX0 decoder without a mapper-aware input
callback.

On MSX, engine and common data are expanded once into the writable page-1
segment. Tracks on the same logical page need no decompression: clear
`7D00H..7F8BH`, select the new original Quarth ID, and call INIT. When the
logical page changes, keep engine/common intact, clear `65B5H..7FFFH`, expose
the compressed staging segment in page 2, decode only the new payload, apply
its descriptor patches, and restore the SCC slot before INIT.

Do not use BIOS `ENASLT` to expose mapper RAM in page 2 from this fixed
page-3 runtime. On an expanded RAM slot it can make the resident code and
stack disappear. Select the RAM primary and secondary page-2 bits directly
through `$A8` and `$FFFF`, preserving the page-3 fields. DOS2 `PUT_P2` then
chooses the physical staging segment; restoring the SCC remains a separate
slot operation.

When reading the two QCPX song lookup tables, do not retain the requested song
ID only in register C: the staged-file mapper reader uses C internally. Reload
the requested ID before indexing the song-to-page table. Failing to do so
materializes page 0 for every song while still passing a different original
song ID to INIT, producing repeated or incomplete music.
Keep the page-record scan countdown in resident state as well, rather than
register B. DOS2 mapper calls need not preserve B; a register-only counter
made page 0 work while later logical-page records were scanned unpredictably.

Clear the entire allocated 16K destination segment before native QCPX
materialization. DOS2 mapper allocation does not promise zero-filled RAM, and
Quarth INIT does not initialize every byte in its relocated work area. The
host materializer already performs this clear with `memset`; omitting it on
MSX left `FFH` state that enabled only one SCC channel or produced hanging
tones.

The complete-page INIT wrapper must preserve AF around the reset call at
`4000H`. Reset changes A, while the selector at `4003H` expects the original
Quarth song ID in A. Without `PUSH AF`/`POP AF`, every request selected song 7;
on pages not containing song 7 this degraded into invalid pointers and
hanging or single-channel output.

## Salamander complete-page conversion

Salamander starts as one `6000H..C046H` KSCC load image. The useful fixed
engine is `6000H..73C7H`, with the original KSS wrappers at `C000H..C046H`;
music resources occupy `73C8H..BFFFH`. Runtime state originally at
`E000H..E1EAH` is relocated to `7E00H..7FEAH`.

`tools/build_salamander_complete_page.py` builds the private uncompressed
`SCPX` container. It relocates the wrapper to `5F80H`, exposes INIT at
`5F97H` and PLAY at `5FC3H`, and creates one logical page for each required
track in `vigamup/salamander.trackinfo`:

```text
new IDs 0..13 -> original IDs 24,26,27,28,29,30,31,32,33,38,39,40,41,42
page 1 4000H..5F7FH  selected relocated music payload
page 1 5F80H..5FFFH  wrappers and direct PSG gateway
page 1 6000H..74AFH  engine, descriptors and common event data
page 1 7E00H..7FEAH  writable engine state
page 2                 real SCC cartridge slot
```

The per-song streams are relocated independently. Stream commands `F9H` and
`FDH` carry a word at `+1`; `FBH` and `FCH` carry a count byte followed by a
word at `+2`. Descriptor words are stored as absolute offsets within the
materialized page and therefore need a different patch routine from Quarth's
common-block-relative patches.

`tools/build_salamander_complete_page_compressed.py` converts SCPX to `SCPZ`.
It stores the sparse 16K template once and ZX0-compresses each track payload
separately. The current files are 31,958 bytes for SCPX and 13,469 bytes for
SCPZ. On a track change the DOS2 player preserves the engine, clears
`4000H..5F7FH` plus `7E00H..7FEAH`, decodes the selected payload, reapplies
its absolute pointer patches, restores the SCC slot, and calls INIT.

A crucial DOS2 portability rule came from this engine: direct BIOS addresses
are not safe merely because page 0 is left unchanged. Under DOS2, page 0 can
be DOS/RAM rather than the BIOS slot. Salamander called/jumped directly to
BIOS `WRTPSG` at `0093H`; on the native player this returned to COM entry
`0100H`, repeatedly printing the loader phases and never reaching PLAY. The
builder now redirects all ten `0093H` references to a page-1-resident gateway
at `5FC7H` that writes ports `A0H/A1H` directly. For future packs, classify
all calls below `0100H` as slot-sensitive dependencies and replace them with
resident gateways or correct interslot calls.

Host libkss materializes both SCPX and SCPZ into ordinary writable 16K banks.
The native DOS2 player materializes only the selected logical page into one
allocated physical segment. Track 5 (original ID 30, “Burn the Wind”) is the
main regression case; all 14 contiguous host IDs pass playback startup, and
the compressed native track-5 test reaches INIT once, continues through PLAY,
and produces PSG/SCC audio.

## Engine/player mapping contract

An engine that will run from an MSX-DOS2 player must expose a logical interface
and leave physical mapper allocation to the player:

ENGINE_INIT
ENGINE_PLAY
ENGINE_STOP
ENGINE_MAP_DATA

For the current Quarth image these entry points are 52BBH, 52DFH, and the safe
stop stub at 52E3H. ENGINE_MAP_DATA is the player-installed user RST 28H
gateway. Quarth puts a logical bank number in A and executes RST 28H; it no
longer emits OUT (FEH),A with a physical segment number. The two-byte RST
28H/NOP replacement preserves the wrapper instruction width. The player saves
and restores the original four bytes at 0028H when playback stops.

The player owns the rest of the operation:

1. allocate physical mapper segments through MSX-DOS2/EXTBIO while keeping
   the resident player in the fixed page-3 TPA area;
2. load and decompress logical data maps into those segments;
3. maintain the logical-to-physical table;
4. select the mapper RAM slot for the target window, then map the requested
   physical segment into it with PUT_Pn;
5. keep the real SCC slot accessible while the RAM music window is active;
6. call ENGINE_STOP, silence the chips, restore the RST 28H vector, interrupt,
   and MSX-DOS2
   mapper layout, and release the allocated segments on exit.

The host kssplayer exercises the same abstraction with --mapper-base N. This
changes where bank 0 is materialized in the emulated mapper while the Quarth
engine continues to request logical banks 0 and 1:

SDL_AUDIODRIVER=dummy build/kssplayer --mapper-base 9 \
  --song 0 --seconds 10 vigamup/extracted/quarth_16k.kss

When adapting another pack, first make the uncompressed repacker use this
gateway and validate it with a deliberately nonzero mapper base. Only then
add compression. The compressed builder must preserve the same logical map
requests, and its MSX loader must decompress into the segments allocated by
the player rather than choosing fixed segment numbers in the engine.

The native MSX adapter has stricter limits than the host VM: it is currently
a specialized 16K-bank path, not a generic DOS2 KSS executor. A DOS2 player
must pass its PUT_P0/PUT_P1/PUT_P2 entrypoints into the resident mapper
service; direct FEH writes from the engine or resident materialization path
bypass DOS2 bookkeeping. PUT_P2 does not select the mapper RAM slot, so the
player must select that slot separately. An 8K KSS cannot be represented by
ordinary 16K RAM-mapper pages without a software copy/emulation strategy.
Page-2 RAM and a separate SCC cartridge cannot occupy the same page on
standard hardware: use page 1 for data, combined hardware, or an SCC-write
intercept/output layer. The resident player must also save/restore the RST
28H and H.TIMI bytes before returning to DOS2.

## Workflow for a fresh AI context

Use this order for a new Konami pack:

1. Read this file and `MERGING_ENGINES_LESSONS.md` before modifying code.
2. Run `tools/extract_kss_assets.py` and inspect the generated `.txt`,
   `.engine`, and `.tracks` files. Check whether the music is embedded in the
   initial load or already in mapper banks.
3. Treat the `.trackinfo` file as the authoritative real-track set. Do not
   infer required IDs from every descriptor or from the nominal `0..255`
   KSS range.
4. Run `build/kss_track_analyzer <directory> <game>` on the original image to
   record the bank selectors used by each real track. Long trackinfo durations
   can make this take minutes; use a shorter trace harness during iteration.
5. Disassemble the executable portion with `z80dasm`. Find direct absolute
   references, then inspect the targets: a table target often contains a
   second layer of pointers. Also inventory calls below `0100H`: direct BIOS
   entry addresses may work in libkss but fail under DOS2 slot/page-0 layout.
6. Build a per-game layout manifest containing the original header, engine
   segments, common tables, stream ranges, bank groups, and patch sites. Keep
   the manifest separate from the generic packer.
7. Pack only the real tracks. Prefer a layout that minimizes duplicated
   common data while keeping every bank self-contained. Count used bytes and
   fixed-bank padding before judging the result size.
8. Patch bank selectors, executable operands, nested pointers, descriptor
   pointers, and stream control-flow operands. Use signature checks and refuse
   unexpected input bytes.
9. Compare original and repacked chip-write traces for every real track. A
   useful fast fingerprint is the ordered `(register, value)` stream for PSG
   ports `A0H/A1H`, followed by SCC register/memory writes. Stop at the first
   divergence and inspect the Z80 state there.
10. Only after the raw banked image is correct, add optional KSSX compression.
    The compressed player should materialize exactly the same normal engine
    image and bank contents before playback.
11. Verify the engine/player contract explicitly: no engine OUT (FEH),A
    writes may contain physical mapper allocation decisions; all data-window
    changes must go through ENGINE_MAP_DATA.
    On MSX, verify that the player-side physical write reaches DOS2 PUT_P1 or
    PUT_P2; a trace showing the handler itself writing FEH is a failure. Also
    verify that the patched RST 28H site is in page 1 or fixed page 3, never
    in the page-2 bank window.

The reusable tooling should evolve toward three layers:

```text
generic extractor/analyzer
        |
per-game layout manifest and pointer classification
        |
small strict per-game repacker + common trace comparator
```

The difficult part is not copying bytes; it is classifying which words are
addresses and which words are musical data. A manifest plus an emulation trace
keeps that judgment explicit and gives an empty context enough evidence to
continue without rediscovering the whole engine.
