# KSP format, version 1

KSP is a compact container whose first 32 bytes are a `KSSX` header. The
header retains the normal KSS load, init, play, and device fields, but the
KSS load image is not duplicated in the file. KSP-aware players materialize
that image from the `ENGN`, `SONG`, and `EDES` chunks. KSS-only tools cannot
play the compact layout by themselves.

KSP-aware tools find the directory by reading the fixed 24-byte trailer at
EOF. All KSP offsets are absolute file offsets.

All integers are little-endian.

## Trailer

```text
00  KSP1             4 bytes
04  trailer_size     u16, 24
06  format_minor     u16, 0
08  directory_offset u32
0C  directory_size   u32
10  directory_crc32  u32, header plus entries
14  flags            u32, currently 0
```

## Directory

The directory begins with:

```text
00  KDIR             4 bytes
04  header_size      u16, 16
06  entry_size       u16, 32
08  entry_count      u32
0C  flags            u32, currently 0
```

Each 32-byte entry contains a FourCC, ID, absolute file offset, packed and
unpacked sizes, CRC32 of the unpacked data, compression ID, flags, and an
auxiliary value. Version 1 defines these compression IDs:

- `0`: uncompressed;
- `1`: forward ZX0 stream (current ZX0 format, non-classic mode).

The packer supports ZX0 only for `ENGN` and `SONG` chunks, and leaves it
disabled unless explicitly requested. `MWK `, `EDES`, and `META` remain
uncompressed. Compression is chunk-local so a player can seek directly to a
selected song. `packed_size` describes the stored ZX0 stream, while
`unpacked_size` and `crc32` describe the original bytes.
The 32-byte KSSX header is copied byte-for-byte and is never compressed or
rewritten. Its load image is reconstructed when needed; it is not stored
between the header and the first chunk.

The initial required chunks are:

- `ENGN`, ID 0: supplied Z80 engine binary;
- `SONG`, ID 0: supplied MBWave song/resource binary. MBWave loaders compact
  its pattern blocks when materializing the KSS image;
- `EDES`, ID 0: the 36-byte `KED1` engine descriptor.

`META`, ID 0 is optional UTF-8 text using `key=value` lines.

The packer and validator deliberately require a KSSX header and the three
required chunks. The packer accepts the engine descriptor as a simple
`key=value` text file; it writes the compact `KED1` binary into the KSP. A
supplied KSS prefix contributes only its first 32-byte header; its load image
is discarded because the engine and song are stored in the chunks.

Use `--zx0` with `mbwave2ksp` or `kspack` to opt in to compression. The DMV
batch converter exposes the same option; its default output remains
uncompressed.

`kspmaterialize INPUT.KSP OUTPUT.KSS` reconstructs the full runtime KSS image
for tools that need a conventional KSS file.

`kspack --engine-msx-bin` accepts the standard seven-byte MSX binary header,
checks its load/end/execute addresses against `EDES`, and stores only the
engine payload.
