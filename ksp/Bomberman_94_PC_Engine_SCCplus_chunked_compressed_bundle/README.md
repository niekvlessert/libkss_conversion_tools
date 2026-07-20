# Bomberman '94 PC Engine to SCC+ KSP prototype

This is a generated 32-track HuC6280-to-SCC+ KCPX/KSP prototype.

- Public tracks: 32
- Internal complete-page overlays: 54
- Z80 replay engine: 629 bytes at 4000H
- Track/chunk overlay: 4800H onward
- Largest unsplit source block: 47507 bytes
- Output size: 465028 bytes
- SHA-256: `86f3609abc8a4ea0750e84c5979d129d0989d50036391f2552d649f2f70dcde6`

## Why this pack is chunked

Several long Bomberman '94 area, boss, staff-roll and battle tracks exceed the 16 KB complete-page budget. The largest unsplit event block is 47,507 bytes. This pack splits those streams at event boundaries. The fixed engine and its state remain at 4000H-47FFH, while a fixed page-3 gateway replaces only the 4800H-7FFFH sparse overlay.

## Mapping

- HuC6280 voices 1-5 -> five independent SCC+ voices.
- HuC6280 voice 6 -> MSX PSG tone A.
- HuC6280 noise/DDA -> approximate MSX PSG noise-B envelope.
- Stereo is folded to mono.
- Playback events are sampled at 60 Hz.

The trace contained 0 DDA sample writes across all tracks, so no DDA sample stream was discarded. HuC6280 noise, when used, is approximated through the MSX PSG noise channel.

The source M3U provides complete durations but no explicit loop offsets. This prototype therefore plays the captured sequence once and stops rather than inventing loop points.

## Player requirement

This file is **not compatible with the current unpatched KSPPLAY.COM** for complete playback. Apply `kspplayer_chunked_sccplus.patch` and rebuild the player. The patch adds a fixed D8A0H gateway which materializes only the requested internal overlay while preserving the engine and runtime state.

## Validation status

- All 54 page records fit in 4800H-7FFFH.
- Every HSC1 event stream was parsed and boundary-validated.
- Every continuation page index and public-song mapping was validated.
- The generated KCPX container was independently reparsed after creation.
- The Z80 engine was instruction-emulated through all 32 tracks and all 54 page overlays.
- 5,483,585 executed instructions matched the independent unsplit event-stream reference.
- Audible testing in openMSX or on a real SD Snatcher/Snatcher Sound Cartridge remains required.
