#!/usr/bin/env python3
"""Build a compact Nemesis 3 KSSX image with player-side bank expansion.

The engine and three physical banks are ZX0-compressed in the file, but the
player expands the banks once before playback. The original engine therefore
keeps its normal instantaneous 16K mapper writes.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


HEADER_SIZE = 0x20
SOURCE_HEADER_SIZE = 0x10
LOAD_ADDRESS = 0x0200
ENGINE_ADDRESS = 0x5FC0
PLAY_ADDRESS = 0x5FE0
ENGINE_SIZE = 0x2040
MAX_LOAD_SIZE = 0xFE00
N3ZX_HEADER = b"N3ZX"

ZX0_DECODER = bytes.fromhex(
    "01 ff ff c5 03 3e 80 cd 35 00 ed b0 87 38 0d "
    "cd 35 00 e3 e5 19 ed b0 e1 e3 87 30 eb c1 0e "
    "fe cd 35 00 0c c8 41 4e 23 cb 18 cb 19 c5 01 "
    "01 00 d4 3d 00 03 18 dd 0c 87 20 03 7e 23 17 "
    "d8 87 cb 11 cb 10 18 f2"
)
ZX0_DECODER_SIZE = len(ZX0_DECODER)


def get_word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def parse_trackinfo(path: Path) -> list[tuple[int, str, int, int, bool]]:
    tracks = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split(",", 4)
        if len(fields) < 3:
            continue
        try:
            track_id = int(fields[0].strip())
            seconds = int(fields[2].strip())
        except ValueError:
            continue
        fade = 0
        loop = False
        if len(fields) >= 4 and fields[3].strip():
            fade = max(0, int(fields[3].strip()))
        if len(fields) == 5:
            loop = fields[4].strip().lower().startswith("y")
        tracks.append((track_id, fields[1].strip(), max(1, seconds), fade, loop))
    if not tracks:
        raise ValueError(f"no tracks found in {path}")
    if len(tracks) > 256:
        raise ValueError("too many tracks for a KSSX song table")
    return tracks


def compress(packer: Path, input_path: Path, output_path: Path) -> bytes:
    subprocess.run([str(packer), str(input_path), str(output_path)], check=True)
    return output_path.read_bytes()


def relocate_decoder(decoder: bytearray, address: int) -> None:
    put_word(decoder, 8, address + 0x35)
    put_word(decoder, 16, address + 0x35)
    put_word(decoder, 32, address + 0x36)
    put_word(decoder, 48, address + 0x3D)


def build_info(tracks: list[tuple[int, str, int, int, bool]]) -> bytes:
    titles = [f"Nemesis 3 [#{track_id}] - {title}" for track_id, title, _, _, _ in tracks]
    size = 0x10
    for title in titles:
        size += 10 + len(title.encode("utf-8")) + 1
    info = bytearray(size)
    info[:4] = b"INFO"
    put_word(info, 8, len(tracks))
    offset = 0x10
    for song, ((track_id, _, seconds, fade, loop), title) in enumerate(zip(tracks, titles)):
        title_bytes = title.encode("utf-8") + b"\0"
        info[offset] = song
        info[offset + 1] = 1 if loop else 0
        put_word(info, offset + 2, (seconds * 1000) & 0xFFFF)
        put_word(info, offset + 4, (seconds * 1000) >> 16)
        put_word(info, offset + 6, (fade * 1000) & 0xFFFF)
        put_word(info, offset + 8, (fade * 1000) >> 16)
        offset += 10
        info[offset : offset + len(title_bytes)] = title_bytes
        offset += len(title_bytes)
    put_word(info, 4, len(info) - 0x10)
    return bytes(info)


def build(source: Path, trackinfo: Path, destination: Path, packer: Path) -> None:
    source_data = source.read_bytes()
    if source_data[:4] != b"KSCC":
        raise ValueError("source must be the patched KSCC image")
    if get_word(source_data, 4) != 0x5FC0 or get_word(source_data, 6) != ENGINE_SIZE:
        raise ValueError("source is not the expected Nemesis 3 16K image")
    if source_data[0x0C] != 9 or source_data[0x0D] != 3:
        raise ValueError("source must contain three 16K Nemesis 3 banks")

    # Keep the patched 16K engine byte-for-byte. In particular, retain its
    # original OUT (FE),A selectors; the player will materialize the physical
    # banks before this engine starts.
    engine = source_data[SOURCE_HEADER_SIZE : SOURCE_HEADER_SIZE + ENGINE_SIZE]
    bank_data = source_data[SOURCE_HEADER_SIZE + ENGINE_SIZE :]
    if len(bank_data) != 3 * 0x4000:
        raise ValueError("source must contain exactly three 16K banks")
    banks = [bank_data[index * 0x4000 : (index + 1) * 0x4000] for index in range(3)]
    common_lower = banks[0][:0x2000]
    if any(bank[:0x2000] != common_lower for bank in banks[1:]):
        raise ValueError("Nemesis 3 banks do not share the expected lower 8K")

    tracks = parse_trackinfo(trackinfo)
    with tempfile.TemporaryDirectory(prefix="nemesis3-zx0-") as temporary:
        temporary_path = Path(temporary)
        initial_bank_path = temporary_path / "lower_plus_upper0.raw"
        upper1_path = temporary_path / "upper1.raw"
        upper2_path = temporary_path / "upper2.raw"
        engine_path = temporary_path / "engine.raw"
        initial_bank_zx0_path = temporary_path / "lower_plus_upper0.zx0"
        upper1_zx0_path = temporary_path / "upper1.zx0"
        upper2_zx0_path = temporary_path / "upper2.zx0"
        engine_zx0_path = temporary_path / "engine.zx0"

        initial_bank_path.write_bytes(banks[0])
        upper1_path.write_bytes(banks[1][0x2000:])
        upper2_path.write_bytes(banks[2][0x2000:])

        compressed_initial = compress(packer, initial_bank_path, initial_bank_zx0_path)
        compressed_upper1 = compress(packer, upper1_path, upper1_zx0_path)
        compressed_upper2 = compress(packer, upper2_path, upper2_zx0_path)

        engine_path.write_bytes(engine)
        compressed_engine = compress(packer, engine_path, engine_zx0_path)

    # The compressed banks live in a private tail. They are expanded by the
    # player before VM_init_bank(), not by the music engine during playback.
    compressed_tail = (
        N3ZX_HEADER
        + struct.pack("<HHH", len(compressed_initial), len(compressed_upper1),
                      len(compressed_upper2))
        + compressed_initial
        + compressed_upper1
        + compressed_upper2
    )

    # KSSX song index -> original Nemesis 3 track number, followed by one
    # compressed engine bootstrap. The engine itself is decompressed directly
    # to 5FC0H and then runs with its original 16K bank-switch instructions.
    bootstrap_size = 22
    decoder_address = LOAD_ADDRESS + bootstrap_size
    source_engine_address = decoder_address + ZX0_DECODER_SIZE
    map_address = source_engine_address + len(compressed_engine)
    load_size = bootstrap_size + ZX0_DECODER_SIZE + len(compressed_engine) + len(tracks)
    if LOAD_ADDRESS + load_size > 0x10000 or load_size > MAX_LOAD_SIZE:
        raise ValueError("compressed load image does not fit in MSX memory")

    bootstrap = bytearray([0xC9] * bootstrap_size)
    # Map the KSSX song index to the original Nemesis 3 track number.
    bootstrap[0:3] = b"!\0\0"
    put_word(bootstrap, 1, map_address)
    bootstrap[3:6] = b"_\x16\0"       # LD E,A / LD D,0
    bootstrap[6] = 0x19                # ADD HL,DE
    bootstrap[7] = 0x7E                # LD A,(HL)
    bootstrap[8] = 0xF5                # PUSH AF
    bootstrap[9] = 0x21                # LD HL,compressed engine
    put_word(bootstrap, 10, source_engine_address)
    bootstrap[12] = 0x11               # LD DE,5FC0H
    put_word(bootstrap, 13, ENGINE_ADDRESS)
    bootstrap[15] = 0xCD               # CALL decoder
    put_word(bootstrap, 16, decoder_address)
    bootstrap[18] = 0xF1               # POP AF
    bootstrap[19] = 0xC3               # JP original engine init
    put_word(bootstrap, 20, ENGINE_ADDRESS)

    decoder = bytearray(ZX0_DECODER)
    relocate_decoder(decoder, decoder_address)
    image = (
        bytes(bootstrap)
        + bytes(decoder)
        + compressed_engine
        + bytes(track[0] for track in tracks)
    )
    assert len(image) == load_size

    info = build_info(tracks)
    header = bytearray(HEADER_SIZE)
    header[:4] = b"KSSX"
    put_word(header, 4, LOAD_ADDRESS)
    put_word(header, 6, load_size)
    put_word(header, 8, LOAD_ADDRESS)
    put_word(header, 10, PLAY_ADDRESS)
    header[0x0C] = 9
    header[0x0D] = 3                 # Three physical 16K banks, materialized by player.
    header[0x0E] = 0x10
    header[0x0F] = 0x04               # RAM mode for the compressed engine bootstrap.
    put_word(header, 0x10, load_size + len(compressed_tail))
    put_word(header, 0x18, 0)
    put_word(header, 0x1A, len(tracks) - 1)

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(bytes(header) + image + compressed_tail + info)
    print(f"wrote {destination}")
    print(f"  compressed engine: {ENGINE_SIZE} -> {len(compressed_engine)} bytes")
    print(f"  compressed initial 16K: 16384 -> {len(compressed_initial)} bytes")
    print(f"  compressed upper bank 1: 8192 -> {len(compressed_upper1)} bytes")
    print(f"  compressed upper bank 2: 8192 -> {len(compressed_upper2)} bytes")
    print(f"  tracks: {len(tracks)}")
    print(f"  load image: {len(image)} bytes")
    print(f"  compressed bank tail: {len(compressed_tail)} bytes")
    print(f"  total: {len(header) + len(image) + len(compressed_tail) + len(info)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path,
                        default=Path("vigamup/extracted/nemesis3_16k.kss"))
    parser.add_argument("destination", nargs="?", type=Path,
                        default=Path("vigamup/extracted/nemesis3_16kb_max_compressed.kss"))
    parser.add_argument("--trackinfo", type=Path,
                        default=Path("vigamup/nemesis3.trackinfo"))
    parser.add_argument("--zx0pack", type=Path, default=Path("build/zx0pack"))
    args = parser.parse_args()
    try:
        build(args.source, args.trackinfo, args.destination, args.zx0pack)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
