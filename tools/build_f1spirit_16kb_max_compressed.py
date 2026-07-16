#!/usr/bin/env python3
"""Build a compact, MSX-executable compressed F-1 Spirit KSSX image."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


HEADER_SIZE = 0x20
SOURCE_HEADER_SIZE = 0x10
LOAD_ADDRESS = 0x0200
ENGINE_ADDRESS = 0x5F00
PLAY_ADDRESS = 0x5F80
BANK_ADDRESS = 0x8000
MAX_LOAD_SIZE = 0xFE00

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
    titles = [f"F-1 Spirit [#{track_id}] - {title}" for track_id, title, _, _, _ in tracks]
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
        raise ValueError("source must be the original KSCC image")
    if get_word(source_data, 4) != ENGINE_ADDRESS or get_word(source_data, 6) != 0x2100:
        raise ValueError("source is not the expected F-1 Spirit image")
    if source_data[0x0C] != 14 or source_data[0x0D] != 1:
        raise ValueError("source must be the patched 16K F-1 Spirit KSS")

    load_size = get_word(source_data, 6)
    engine = bytearray(source_data[SOURCE_HEADER_SIZE : SOURCE_HEADER_SIZE + load_size])
    bank = source_data[SOURCE_HEADER_SIZE + load_size :]
    if len(bank) != 0x4000:
        raise ValueError("source must contain one 16K bank")
    # Keep the original bank-selector instructions. With no physical KSS
    # banks, the patched KSSX player maps selector 14 back to writable main
    # RAM, preserving the engine's original timing and state transitions.

    tracks = parse_trackinfo(trackinfo)
    with tempfile.TemporaryDirectory(prefix="f1spirit-zx0-") as temporary:
        temporary_path = Path(temporary)
        engine_path = temporary_path / "engine.raw"
        bank_path = temporary_path / "bank.raw"
        engine_zx0_path = temporary_path / "engine.zx0"
        bank_zx0_path = temporary_path / "bank.zx0"
        engine_path.write_bytes(engine)
        bank_path.write_bytes(bank)
        compressed_engine = compress(packer, engine_path, engine_zx0_path)
        compressed_bank = compress(packer, bank_path, bank_zx0_path)

    bootstrap_size = 31
    decoder_address = LOAD_ADDRESS + bootstrap_size
    source_engine_address = decoder_address + ZX0_DECODER_SIZE
    source_bank_address = source_engine_address + len(compressed_engine)
    map_address = source_bank_address + len(compressed_bank)
    load_size = bootstrap_size + ZX0_DECODER_SIZE + len(compressed_engine) + len(compressed_bank) + len(tracks)
    if LOAD_ADDRESS + load_size > 0x10000 or load_size > MAX_LOAD_SIZE:
        raise ValueError("compressed load image does not fit in MSX memory")

    bootstrap = bytearray([0xC9] * bootstrap_size)
    # Map the KSSX song index to the original F-1 Spirit track number.
    bootstrap[0:3] = b"!\0\0"
    put_word(bootstrap, 1, map_address)
    bootstrap[3:6] = b"_\x16\0"  # LD E,A / LD D,0
    bootstrap[6] = 0x19            # ADD HL,DE
    bootstrap[7] = 0x7E            # LD A,(HL)
    bootstrap[8] = 0xF5            # PUSH AF (preserve mapped song)
    bootstrap[9] = 0x21            # LD HL,compressed engine
    put_word(bootstrap, 10, source_engine_address)
    bootstrap[12] = 0x11           # LD DE,engine destination
    put_word(bootstrap, 13, ENGINE_ADDRESS)
    bootstrap[15] = 0xCD           # CALL decoder
    put_word(bootstrap, 16, decoder_address)
    bootstrap[18] = 0x21           # LD HL,compressed bank
    put_word(bootstrap, 19, source_bank_address)
    bootstrap[21] = 0x11           # LD DE,bank destination
    put_word(bootstrap, 22, BANK_ADDRESS)
    bootstrap[24] = 0xCD           # CALL decoder
    put_word(bootstrap, 25, decoder_address)
    bootstrap[27] = 0xF1           # POP AF
    bootstrap[28] = 0xC3           # JP original engine init
    put_word(bootstrap, 29, ENGINE_ADDRESS)

    decoder = bytearray(ZX0_DECODER)
    relocate_decoder(decoder, decoder_address)
    image = bytes(bootstrap) + bytes(decoder) + compressed_engine + compressed_bank
    image += bytes(track[0] for track in tracks)
    assert len(image) == load_size

    info = build_info(tracks)
    header = bytearray(HEADER_SIZE)
    header[:4] = b"KSSX"
    put_word(header, 4, LOAD_ADDRESS)
    put_word(header, 6, load_size)
    put_word(header, 8, LOAD_ADDRESS)
    put_word(header, 10, PLAY_ADDRESS)
    header[0x0C] = 14
    header[0x0D] = 0  # No physical banks: the loader expands into main RAM.
    header[0x0E] = 0x10
    # MSX RAM mode makes 8000H-BFFFH writable so the loader can expand the
    # compressed 16K bank there.  The patched KSSX player keeps SCC enabled.
    header[0x0F] = 0x04
    put_word(header, 0x10, load_size)
    put_word(header, 0x18, 0)
    put_word(header, 0x1A, len(tracks) - 1)

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(bytes(header) + image + info)
    print(f"wrote {destination}")
    print(f"  compressed engine: {len(engine)} -> {len(compressed_engine)} bytes")
    print(f"  compressed 16K bank: {len(bank)} -> {len(compressed_bank)} bytes")
    print(f"  tracks: {len(tracks)}")
    print(f"  load image: {len(image)} bytes")
    print(f"  total: {len(header) + len(image) + len(info)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path,
                        default=Path("vigamup/extracted/f1spirit_16k.kss"))
    parser.add_argument("destination", nargs="?", type=Path,
                        default=Path("vigamup/extracted/f1spirit_16kb_max_compressed.kss"))
    parser.add_argument("--trackinfo", type=Path,
                        default=Path("vigamup/f1spirit.trackinfo"))
    parser.add_argument("--zx0pack", type=Path, default=Path("build/zx0pack"))
    args = parser.parse_args()
    try:
        build(args.source, args.trackinfo, args.destination, args.zx0pack)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
