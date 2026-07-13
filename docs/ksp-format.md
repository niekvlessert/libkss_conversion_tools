# KSP format, version 1

KSP is an append-only container around a valid `KSSX` image. The KSSX bytes
remain unchanged at the beginning of the file. KSP-aware tools find the
directory by reading the fixed 24-byte trailer at EOF; KSS-only tools can use
the prefix as an ordinary KSSX image.

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
auxiliary value. Version 1 supports compression ID zero only.

The initial required chunks are:

- `ENGN`, ID 0: supplied Z80 engine binary;
- `SONG`, ID 0: supplied MBWave song/resource binary;
- `EDES`, ID 0: the 36-byte `KED1` engine descriptor.

`META`, ID 0 is optional UTF-8 text using `key=value` lines.

The packer and validator deliberately require a KSSX prefix and the three
required chunks. The initial packer accepts the engine descriptor as a simple
`key=value` text file; it writes the compact `KED1` binary into the KSP. Supplied
engine and song binaries are input files and are not part of this repository.

`kspack --engine-msx-bin` accepts the standard seven-byte MSX binary header,
checks its load/end/execute addresses against `EDES`, and stores only the
engine payload.
