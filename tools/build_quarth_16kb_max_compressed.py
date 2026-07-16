#!/usr/bin/env python3
"""Build a compact Quarth KSSX image with player-side bank expansion.

The raw Quarth repacker produces two ordinary 16K banks. This builder keeps
the relocated engine and a small song-ID bootstrap in the KSS load image,
compresses the engine with ZX0, and stores a private QRTX tail containing one
compressed copy of the shared bank prefix plus the two bank-specific data
regions. The libkss player reconstructs the normal two-bank image before the
Z80 starts. The engine's bank selectors remain logical ENGINE_MAP_DATA
requests; the player chooses the physical mapper segments.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]


HEADER_SIZE = 0x20
SOURCE_HEADER_SIZE = 0x10
LOAD_ADDRESS = 0x0200
ENGINE_ADDRESS = 0x4000
ENGINE_INIT_ADDRESS = 0x52BB
PLAY_ADDRESS = 0x52DF
MAX_LOAD_SIZE = 0xFE00

BANK_SIZE = 0x4000
BANK_COUNT = 2
BANK_OFFSET = 0
COMMON_SIZE = 0x12D1
BANK0_STREAM_SIZE = 0x2BA9
BANK1_STREAM_SIZE = 0x1E9C
BANK_DATA_SIZE = 0x2D29  # Shared data footprint, including the A0F0H copy.
COMMON_SOURCE_ADDRESS = 0x52A2
VOLUME_TABLE_SOURCE_ADDRESS = 0x642A
A0F0_SOURCE_ADDRESS = 0xA0F0
A0F0_SIZE = 0x0180

TRACKS = (
    5,
    3,
    9,
    10,
    11,
    4,
    12,
    13,
    14,
    6,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    7,
    8,
)

QRTX_HEADER = b"QRTX"

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
    if tuple(track[0] for track in tracks) != TRACKS:
        raise ValueError(
            "Quarth trackinfo must contain the expected 19 tracks in order: "
            + ",".join(map(str, TRACKS))
        )
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
    titles = [f"Quarth [#{track_id}] - {title}" for track_id, title, _, _, _ in tracks]
    size = 0x10
    for title in titles:
        size += 10 + len(title.encode("utf-8")) + 1
    info = bytearray(size)
    info[:4] = b"INFO"
    put_word(info, 8, len(tracks))
    offset = 0x10
    for song, ((_, _, seconds, fade, loop), title) in enumerate(zip(tracks, titles)):
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


def common_source_offset(common: bytes, track_id: int) -> int:
    value = get_word(common, 2 * track_id)
    if value >= 0x8000:
        value = COMMON_SOURCE_ADDRESS + value - 0x8000
    if not COMMON_SOURCE_ADDRESS <= value < COMMON_SOURCE_ADDRESS + COMMON_SIZE:
        raise ValueError(f"track {track_id} descriptor is outside Quarth common data")
    return value - COMMON_SOURCE_ADDRESS


def make_shared_bank_parts(
    bank0: bytes, bank1: bytes
) -> tuple[bytes, bytes, bytes]:
    """Merge the bank-specific descriptor patches into one common prefix.

    The raw repacker patches descriptors only in the bank that owns their
    streams. For the compressed image, both banks use the same common prefix,
    so copy each descriptor from its owning bank. The bank-specific data then
    uses the same offsets in both banks; bank 1 gets a zero gap before its
    shared A0F0H dependency.
    """
    common = bytearray(bank0[:COMMON_SIZE])
    for track_id in TRACKS:
        descriptor = common_source_offset(common, track_id)
        descriptor_end = common_source_offset(common, track_id + 1)
        if descriptor_end <= descriptor + 2 or descriptor_end > COMMON_SIZE:
            raise ValueError(f"invalid Quarth descriptor bounds for track {track_id}")
        owner = bank0 if track_id <= 14 else bank1
        common[descriptor:descriptor_end] = owner[descriptor:descriptor_end]

    # Place A0F0H at one address in both banks. Bank 1's stream begins at the
    # same offset, then leaves a zero gap before this dependency.
    a0f0_destination = 0x8000 + COMMON_SIZE + BANK0_STREAM_SIZE
    put_word(common, VOLUME_TABLE_SOURCE_ADDRESS - COMMON_SOURCE_ADDRESS, a0f0_destination)

    bank0_data = bytearray(
        bank0[COMMON_SIZE : COMMON_SIZE + BANK0_STREAM_SIZE + A0F0_SIZE]
    )

    bank1_data = bytearray(BANK_DATA_SIZE)
    bank1_data[:BANK1_STREAM_SIZE] = bank1[COMMON_SIZE : COMMON_SIZE + BANK1_STREAM_SIZE]
    a0f0_offset = A0F0_SOURCE_ADDRESS - 0x906D
    bank1_data[BANK0_STREAM_SIZE : BANK0_STREAM_SIZE + A0F0_SIZE] = bank1[
        COMMON_SIZE + a0f0_offset : COMMON_SIZE + a0f0_offset + A0F0_SIZE
    ]
    return bytes(common), bytes(bank0_data), bytes(bank1_data)


def validate_source(source_data: bytes) -> tuple[bytes, bytes, bytes, bytes]:
    if source_data[:4] != b"KSCC":
        raise ValueError("source must be the raw repacked KSCC image")
    header = source_data[:SOURCE_HEADER_SIZE]
    if (
        get_word(header, 4),
        get_word(header, 8),
        get_word(header, 10),
    ) != (0x4000, ENGINE_INIT_ADDRESS, PLAY_ADDRESS):
        raise ValueError("source is not the expected Quarth 16K image")
    if get_word(header, 6) < 0x12E3 or get_word(header, 6) > 0x12E4:
        raise ValueError("source has an unexpected Quarth load size")
    if header[0x0C] != BANK_OFFSET or header[0x0D] != BANK_COUNT:
        raise ValueError("source must contain two 16K Quarth banks at offset 0")

    load_size = get_word(header, 6)
    expected_size = SOURCE_HEADER_SIZE + load_size + BANK_COUNT * BANK_SIZE
    if len(source_data) != expected_size:
        raise ValueError(
            f"unexpected source size {len(source_data)} (expected {expected_size})"
        )

    engine = source_data[SOURCE_HEADER_SIZE : SOURCE_HEADER_SIZE + load_size]
    bank_data = source_data[SOURCE_HEADER_SIZE + load_size :]
    bank0 = bank_data[:BANK_SIZE]
    bank1 = bank_data[BANK_SIZE:]
    common, bank0_data, bank1_data = make_shared_bank_parts(bank0, bank1)
    return engine, common, bank0_data, bank1_data


def build(source: Path, trackinfo: Path, destination: Path, packer: Path) -> None:
    source_data = source.read_bytes()
    engine, common, bank0_data, bank1_data = validate_source(source_data)
    tracks = parse_trackinfo(trackinfo)

    work = ROOT / "tmp" / "quarth-zx0-build"
    work.mkdir(parents=True, exist_ok=True)
    inputs = {
        "engine": engine,
        "common": common,
        "bank0": bank0_data,
        "bank1": bank1_data,
    }
    compressed = {}
    for name, raw in inputs.items():
        raw_path = work / f"{name}.raw"
        zx0_path = work / f"{name}.zx0"
        raw_path.write_bytes(raw)
        compressed[name] = compress(packer, raw_path, zx0_path)

    # The bootstrap maps KSSX song indices 0..18 back to Quarth's original
    # compact descriptor IDs before entering the relocated Quarth INIT.
    bootstrap_size = 22
    decoder_address = LOAD_ADDRESS + bootstrap_size
    compressed_engine_address = decoder_address + ZX0_DECODER_SIZE
    map_address = compressed_engine_address + len(compressed["engine"])
    load_size = bootstrap_size + ZX0_DECODER_SIZE + len(compressed["engine"]) + len(TRACKS)
    decoder = bytearray(ZX0_DECODER)
    relocate_decoder(decoder, decoder_address)

    bootstrap = bytearray([0xC9] * bootstrap_size)
    bootstrap[0:3] = b"!\0\0"  # LD HL, song map
    put_word(bootstrap, 1, map_address)
    bootstrap[3:6] = b"_\x16\0"  # LD E,A / LD D,0
    bootstrap[6] = 0x19          # ADD HL,DE
    bootstrap[7] = 0x7E          # LD A,(HL)
    bootstrap[8] = 0xF5          # PUSH AF
    bootstrap[9] = 0x21          # LD HL, compressed engine
    put_word(bootstrap, 10, compressed_engine_address)
    bootstrap[12] = 0x11         # LD DE,4000H
    put_word(bootstrap, 13, ENGINE_ADDRESS)
    bootstrap[15] = 0xCD         # CALL decoder
    put_word(bootstrap, 16, decoder_address)
    bootstrap[18] = 0xF1         # POP AF
    bootstrap[19] = 0xC3         # JP relocated Quarth INIT
    put_word(bootstrap, 20, ENGINE_INIT_ADDRESS)

    image = bytes(bootstrap) + bytes(decoder) + compressed["engine"] + bytes(TRACKS)
    assert len(image) == load_size
    if LOAD_ADDRESS + load_size > 0x10000 or load_size > MAX_LOAD_SIZE:
        raise ValueError("compressed Quarth load image does not fit in MSX memory")

    # QRTX: compressed common prefix, followed by the used data portions of
    # logical bank 0 and logical bank 1. The player restores the omitted zero
    # padding and materializes both maps in whichever physical mapper segments
    # it allocated; the engine only requests logical bank numbers.
    compressed_tail = (
        QRTX_HEADER
        + struct.pack(
            "<HHH",
            len(compressed["common"]),
            len(compressed["bank0"]),
            len(compressed["bank1"]),
        )
        + compressed["common"]
        + compressed["bank0"]
        + compressed["bank1"]
    )

    info = build_info(tracks)
    header = bytearray(HEADER_SIZE)
    header[:4] = b"KSSX"
    put_word(header, 4, LOAD_ADDRESS)
    put_word(header, 6, load_size)
    put_word(header, 8, LOAD_ADDRESS)
    put_word(header, 10, PLAY_ADDRESS)
    header[0x0C] = BANK_OFFSET
    header[0x0D] = BANK_COUNT
    header[0x0E] = 0x10
    header[0x0F] = 0x00  # Quarth's original PSG/SCC device flags.
    put_word(header, 0x10, load_size + len(compressed_tail))
    put_word(header, 0x18, 0)
    put_word(header, 0x1A, len(tracks) - 1)

    destination.parent.mkdir(parents=True, exist_ok=True)
    result = bytes(header) + image + compressed_tail + info
    destination.write_bytes(result)
    print(f"wrote {destination}")
    print(f"  compressed engine: {len(engine)} -> {len(compressed['engine'])} bytes")
    print(f"  compressed common: {len(common)} -> {len(compressed['common'])} bytes")
    print(f"  compressed bank 0 data: {len(bank0_data)} -> {len(compressed['bank0'])} bytes")
    print(f"  compressed bank 1 data: {len(bank1_data)} -> {len(compressed['bank1'])} bytes")
    print(f"  tracks: {len(tracks)} (KSSX IDs 0..{len(tracks) - 1})")
    print(f"  load image: {len(image)} bytes")
    print(f"  QRTX tail: {len(compressed_tail)} bytes")
    print(f"  total: {len(result)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source",
        nargs="?",
        type=Path,
        default=Path("vigamup/extracted/quarth_16k.kss"),
    )
    parser.add_argument(
        "destination",
        nargs="?",
        type=Path,
        default=Path("vigamup/extracted/quarth_16kb_max_compressed.kss"),
    )
    parser.add_argument(
        "--trackinfo", type=Path, default=Path("vigamup/quarth.trackinfo")
    )
    parser.add_argument("--zx0pack", type=Path, default=Path("build/zx0pack"))
    args = parser.parse_args()
    try:
        build(args.source, args.trackinfo, args.destination, args.zx0pack)
    except (OSError, ValueError, subprocess.CalledProcessError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
