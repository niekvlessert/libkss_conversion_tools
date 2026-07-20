# Bomberman '94 SCC+ KCPZ pack

- Container: KCPZ version 2
- Public tracks: 32
- Internal overlays: 54
- Original KCPX: 465,028 bytes
- ZX0 KCPZ: 146,983 bytes
- Reduction: 68.39%
- SHA-256: `ad48841b835cec1535a0fc29aa8e49fece18c296d4f88041b6ba368d9c8db64b`
- Compression: standard ZX0 v2; optimal for template/first 18 overlays and quick-mode ZX0 for the remaining overlays

The common 16 KB template and each sparse overlay are separate standard forward ZX0 v2 streams. Every stream was decompressed and compared with its KCPX source. All reconstructed 16 KB pages are byte-identical.

This chunked pack requires `kspplayer_chunked_sccplus_kcpz.patch`. The patch adds an overlay-only KCPZ decoder at the D8A0H gateway, preserving the resident engine and playback state during internal page changes.

## Sparse staging for internal chunks

For each public track, the first page descriptor advertises the packed span of all its contiguous internal chunks. This lets the existing DOS2 sparse loader cache the one or two 16 KB file segments needed by the entire track before playback starts. The ZX0 decoder still stops at the end marker of the selected individual stream.
