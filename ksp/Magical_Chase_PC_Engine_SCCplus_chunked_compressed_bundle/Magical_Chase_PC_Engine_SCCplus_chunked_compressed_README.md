# Magical Chase SCC+ KCPZ pack

- Container: KCPZ version 2
- Public tracks: 19
- Internal overlays: 35
- Original KCPX: 354,452 bytes
- ZX0 KCPZ: 101,790 bytes
- Reduction: 71.28%
- SHA-256: `3f5282ea22acf0b637ee5d205fc1e5c1d96397f18c3807b2eb059659eb0910c3`
- Compression: optimal ZX0 v2 per stream

The common 16 KB template and each sparse overlay are separate standard forward ZX0 v2 streams. Every stream was decompressed and compared with its KCPX source. All reconstructed 16 KB pages are byte-identical.

This chunked pack requires `kspplayer_chunked_sccplus_kcpz.patch`. The patch adds an overlay-only KCPZ decoder at the D8A0H gateway, preserving the resident engine and playback state during internal page changes.

## Sparse staging for internal chunks

For each public track, the first page descriptor advertises the packed span of all its contiguous internal chunks. This lets the existing DOS2 sparse loader cache the one or two 16 KB file segments needed by the entire track before playback starts. The ZX0 decoder still stops at the end marker of the selected individual stream.
