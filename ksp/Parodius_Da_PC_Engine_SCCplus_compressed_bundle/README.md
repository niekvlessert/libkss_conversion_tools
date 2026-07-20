# Parodius Da! PC Engine to SCC+ KSP prototype

This is an experimental KCPX/KSP file for the current `KSPPLAYER.COM` complete-page path.

- Tracks: 38
- Z80 replay engine: 567 bytes at 4000H
- Track overlay: 4800H onward
- Output size: 208387 bytes
- SHA-256: bfdf998ff865cab5db0d9717811eeba94969b05cff6dbd1892c0e6d04bc8a2e6

## Mapping

- HuC6280 voices 1-5 -> the five independent SCC+ voices.
- HuC6280 voice 6 -> MSX PSG tone A.
- HuC6280 DDA/sample percussion and PSG noise -> an approximate PSG noise-B amplitude envelope.
- Stereo balance is folded to mono.
- Playback state is sampled at 60 Hz.

## Hardware/runtime

The engine writes 20H to BFFEH and 80H to B000H, then uses the SCC+ register block at B800H-B8AFH. Use an original Snatcher/SD Snatcher Sound Cartridge or compatible SCC+ hardware in the SCC slot selected by KSPPLAYER.

## Important status

This is a real generated replay container, not a renamed HES file. Its KCPX structure and all 38 overlays were validated host-side. The generated Z80 INIT/PLAY engine was also instruction-emulated through one complete trace cycle for every track (2,478,722 executed Z80 instructions) and matched an independent event-stream decoder. It has not been listened to in openMSX or on real hardware in this environment. DDA percussion is intentionally approximate.

The current KSPPLAYER source should also clear B8AAH-B8AFH in `kcpx_silence`; see `kspplayer_sccplus_silence.patch`.
