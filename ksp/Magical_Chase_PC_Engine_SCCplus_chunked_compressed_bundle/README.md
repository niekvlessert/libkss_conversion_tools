# Magical Chase PC Engine to SCC+ KSP prototype

This is a generated 19-track HuC6280-to-SCC+ KCPX/KSP prototype.

- Public tracks: 19
- Internal complete-page overlays: 35
- Z80 replay engine: 617 bytes at 4000H
- Track/chunk overlay: 4800H onward
- Output size: 354452 bytes
- SHA-256: `6de7b1db0dbe8a2d6bc19ccd582e1a79c6992e3a731cdf83af7fb5f087e1b1c4`

## Why this pack is chunked

Magical Chase changes pitch/volume state far more often than Parodius. Several uncompressed replay streams exceed the 16 KB complete-page budget; the longest source event block is 48,209 bytes. This pack splits those streams at event boundaries. The fixed engine and its state remain at 4000H-47FFH. A small fixed page-3 gateway replaces only the 4800H-7FFFH sparse overlay when playback reaches a chunk boundary.

## Mapping

- HuC6280 voices 1-5 -> five independent SCC+ voices.
- HuC6280 voice 6 -> MSX PSG tone A.
- HuC6280 noise/DDA -> approximate PSG noise-B envelope.
- Stereo is folded to mono.
- Playback events are sampled at 60 Hz.

The trace contained no DDA sample writes for these 19 tracks, so the percussion path is less approximate than in the Parodius prototype.

## Player requirement

This file is **not compatible with the current unpatched KSPPLAY.COM** for complete playback. Apply `kspplayer_chunked_sccplus.patch` and rebuild the player. The patch adds a fixed D8A0H gateway which materializes only the requested internal overlay while preserving the engine and runtime state.

## Validation status

- All 35 page records fit in 4800H-7FFFH.
- Every HSC1 event stream was parsed and boundary-validated.
- Every continuation page index and public-song mapping was validated.
- The generated KCPX container was independently reparsed after creation.
- The Z80 engine was instruction-emulated across all 19 complete tracks and all internal page switches: 4,024,026 instructions matched the unsplit reference streams.
- Audible testing in openMSX or on a real SD Snatcher/Snatcher Sound Cartridge remains required.
