# MoonBlaster Wave engine ABI

This document records the ABI recovered from `dutch_moonsound_veterans/DMV1`
and the supplied OpenMSX trace. It describes the DMV1 `WAVEDRVR.BIN` / Wave
driver v1.17 combination, not every MoonBlaster release.

## Binary and relocation

`WAVEDRVR.BIN` has a standard MSX binary header:

```text
load: 9000H
end:  CFD7H
exec: 9000H
payload: 3FD7H bytes
```

The loader at `9000H` performs the DOS/BIOS setup, then copies:

```text
source:      90B0H
destination: 4000H
length:      3F27H
destination end: 7F26H
```

The copied block begins with the driver marker `AB 00` at `4000H`. It is
position-dependent and should be kept at `4000H` in the first bootstrap.

## Work area and song pointer

The loader clears `DA00H..DA23H`, sets `DA01H..DA03H` to `03H`, and writes the
little-endian pointer `8006H` to `DA04H`. The six-byte `MBMS` signature remains
at `8000H`; the player expects the MWM header immediately after it and copies
its working state from the address in `DA04H` during `MBPLAY`.

The original BASIC/DOS sequence is conceptually:

```text
MBADDR(8006H)
MWMLOAD(song filename)
MBPLAY()
```

For a KSS/KSP bootstrap, disk loading is replaced by copying the selected MWM
image to `8000H`. The small bootstrap at `7F30H` stores `8006H` in `DA04H`
before it calls `MBPLAY`; `MWMLOAD` must not be called from the playback
interrupt or used as a substitute for the host-side load. The host-side
bootstrap also removes each three-byte pattern-block header that `MWMLOAD`
consumes before leaving the pattern data in memory.

## Entry points

The command table embedded after the `AB 00` marker resolves as follows:

```text
MBPLAY   4042H    MBSTOP   4048H    MBCONT   405BH
MBHALT   4061H    MBINIT   4067H    MBALLOC  4068H
MBFREE   4075H    MBBANK1  407BH    MBBANK2  4082H
MBBANK3  4089H    MBADDR   4090H    MBVER    40A3H
MWMLOAD  40BAH    MWKLOAD  418BH    MBFADE   404EH
```

The wrappers use the original BASIC extension calling convention internally.
For a KSS/KSP adapter, the useful call sequence is:

```text
init:  call 7F30H       ; store 8006H in DA04H, then call MBPLAY
play:  call 45E5H       ; internal 60 Hz tick/interrupt body
stop:  call 4048H       ; MBSTOP
```

`45E5H` is the routine whose writes begin at `45E8H/45EEH` in the reference
trace and whose common return is at `46ACH`. Calling `MBPLAY` once and then
calling the tick body from the host avoids relying on the DOS/BASIC interrupt
hook installed by the original loader. Register preservation and argument
behavior of optional commands are not yet part of the KSP ABI.

`MBINIT` resolves to a very small wrapper in this version; the substantial
player/device setup is reached by `MBPLAY` through the internal routine at
`4483H`. This is why the reference trace’s first large MoonSound transaction
appears when playback is started rather than immediately after the binary is
loaded.

The corresponding descriptor values for a raw copied-driver engine are:

```text
engine_type=mbwave
load_address=0x4000
init_address=0x7f30
play_address=0x45e5
stop_address=0x4048
capability_address=0
song_window_address=0x8000
work_address=0xda00
work_size=0x0100
tick_rate_num=60
tick_rate_den=1
minimum_mapper_ram=0
flags=0
```

Create that raw engine from the supplied MSX binary with:

```bash
mbwave_extract WAVEDRVR.BIN mbwave-dmv1.engine
```

The extractor validates the MSX header and the `AB\0` marker before writing
the exact `3F27H`-byte image expected at `4000H`.

For the first KSS/KSP bootstrap, `mbwave2ksp` performs that extraction and
builds the archive in one step. It stores the MWM header, metadata, position
table, and pattern offsets at `8000H`, followed by pattern data with the
loader-only block headers removed. The original raw MWM remains in the KSP
`SONG` chunk. `DA04H` is set to `8006H` so the engine skips the `MBMS`
signature:

```bash
mbwave2ksp --driver WAVEDRVR.BIN --song REARVIEW.MWM \
  --title Rearviewmirror --game DMV1 --output rearview.ksp
```

If the MWM header names a RAM wave kit, pass the matching MWK as well:

```bash
mbwave2ksp --driver WAVEDRVR.BIN --song RESIST.MWM \
  --mwk OPL4MUS.MWK --output resist.ksp
```

The optional kit is stored as an `MWK ` chunk. Tracks whose wave numbers are
all in the YRW801 ROM range do not need an MWK.

The KSS-visible prefix contains the fixed driver at `4000H` and the compact
MWM engine image at `8000H`; the KSP directory also contains the original
`SONG`, `ENGN`, and `EDES` chunks.
This first bootstrap intentionally keeps the KSS-visible copy so it can be
tested independently. Compression and eliminating that transitional duplicate
come after the runtime path is verified.

## MoonSound ports

The DMV1 player uses the standard MoonSound/YMF278B I/O groups:

```text
7EH  wave-register address
7FH  wave-register data
C4H  FM-register address / status reads
C5H  FM-register data
C6H/C7H  wave-side initialization access
```

The player polls `C4H` for device readiness during wave-data transfers. A
desktop backend or MSX bootstrap must therefore provide correct reads as well
as writes. The supplied trace was write-only, so exact status bit timing still
needs a backend-level comparison.

## Timing

In the reference run, code at `45E8H` and `45EEH` writes `04H` to `C4H` and
`81H` to `C5H` every approximately `16.7 ms`. The DMV1 player actually derives
that timer from the MWM base-frequency byte at header offset `1BH` (file offset
`21H`): zero means 60 Hz, one means 50 Hz, and other values use
`1 / (value * 0.0000808) + 0.5`. Some songs can change the value at runtime in
work byte `4E69H`.

The descriptor keeps `tick_rate_num=60` and `tick_rate_den=1` as its fallback,
while the libkss MBWave adapter replaces it with the MWM header value and
follows changes made to `4E69H`. This keeps both the WAV renderer and live
player aligned with the original engine.

## Open items

- classify which MWM/MWK pairs require external wave/sample data;
- confirm the minimal song buffer size and whether all MWM files are
  self-contained;
- implement the first KSS-compatible bootstrap at the fixed `4000H` address;
- add MoonSound status-read tracing and compare register traces against
  OpenMSX;
- verify the same entry points with a second DMV disk/player revision.
