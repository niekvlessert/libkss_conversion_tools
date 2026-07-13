# Dutch MoonSound Veterans inventory

The supplied `dutch_moonsound_veterans` directory is suitable for Phase 0
engine research.

## Reference media

- `DMV1.dsk`, `DMV2.dsk`, and `DMV3.dsk` are runnable disk images.
- `DMVFT.dsk` is the larger full collection.
- The matching ZIP files contain the individual files without requiring a
  disk-image extraction utility.

## First player candidate

`DMV1/WAVEDRVR.BIN` is an MSX binary image:

```text
load address: 9000H
end address:  CFD7H
execute:      9000H
payload:      16343 bytes after the seven-byte MSX header
```

The file begins with the standard `FE load end execute` header. The initial
`kspack --engine-msx-bin` path strips that header after checking it against the
engine descriptor.

## Song/resource candidates

`.MWM` and `.MWK` files begin with the `MBMS` signature. They must still be
classified by the original player before deciding whether one is a song,
instrument set, or another MoonBlaster resource.

## Confirmed player ABI

The supplied trace `openmsx-20260713-185232.log` was captured while the
original player was producing audio. The trace script initially printed the
complete Z80 `BC` I/O address; the hardware port is its low byte. For example,
`PORT=5C6` means MoonSound port `C6`, not a separate port named `5C6`.

`WAVEDRVR.BIN` is an MSX binary that:

- clears the player work area at `DA00H..DA23H`;
- initializes the song pointer at `DA04H` to `8006H` (after the `MBMS` signature);
- copies `3F27H` bytes from `90B0H` to `4000H`;
- executes the copied driver through its BASIC-extension entry points.

The copied image starts with the `AB\0` driver marker. Its command table gives
these useful entry points:

| Command | Entry |
|---|---:|
| `MBPLAY` | `4042H` |
| `MBSTOP` | `4048H` |
| `MBCONT` | `405BH` |
| `MBHALT` | `4061H` |
| `MBINIT` | `4067H` |
| `MBALLOC` | `4068H` |
| `MBFREE` | `4075H` |
| `MBBANK1` | `407BH` |
| `MBBANK2` | `4082H` |
| `MBBANK3` | `4089H` |
| `MBADDR` | `4090H` |
| `MBVER` | `40A3H` |
| `MWMLOAD` | `40BAH` |
| `MWKLOAD` | `418BH` |
| `MBFADE` | `404EH` |

For a KSS/KSP host, the important consequence is that the song can be placed
at `8000H`, `DA04H` must point to `8006H`, and the host can call `MBPLAY` without
running the DOS-side `MWMLOAD` command. The host must also remove the three-byte
header before each on-disk pattern block, because `MWMLOAD` consumes those
headers before playback. The exact raw `.MWM` resource dependency is preserved
in the KSP `SONG` chunk; the driver itself does not perform DOS file I/O during
normal playback.

## Confirmed MoonSound I/O

The exact copied player and the audio trace agree on this map:

- `7EH`: OPL4 wave-register address;
- `7FH`: OPL4 wave-register data;
- `C4H`: OPL4 FM-register address and status reads;
- `C5H`: OPL4 FM-register data;
- `C6H/C7H`: one-time wave-side initialization access in this driver.

The trace shows repeated `C4H=04H, C5H=81H` writes from player code at
`45E8H/45EEH` every approximately `16.7 ms`, establishing a 60 Hz playback
update path for this run. The player also polls `C4H` while transferring wave
data, so a host implementation must model status reads rather than only
recording writes.

The port map and tick behavior are now confirmed for the DMV1 player. The
remaining ABI work is to make the KSS/KSP bootstrap reproduce the original
`DA04H` song setup and call sequence, then compare its OPL4 register trace with
this reference run.
