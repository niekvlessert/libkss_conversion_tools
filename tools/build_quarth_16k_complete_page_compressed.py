#!/usr/bin/env python3
"""Compress the proven Quarth QCPX complete-page container as QCPZ.

QCPZ retains the QCPX song maps, relocated addresses and patch records, but
ZX0-compresses the shared engine, shared common block and each page payload.
Every compressed stream is kept inside one 16K file-staging boundary so the
native MSX player can expose the source in page 2 and decode directly into
the single writable complete-page segment in page 1.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 0x20
TAIL_FILE_OFFSET = 0x21
STAGE_SIZE = 0x4000
QCPZ_HEADER_SIZE = 16
SONG_MAP_SIZE = 512
PAGE_DESCRIPTOR_SIZE = 10


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def compress(packer: Path, name: str, raw: bytes) -> bytes:
    work = ROOT / "tmp" / "quarth-qcpz-build"
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"{name}.raw"
    destination = work / f"{name}.zx0"
    source.write_bytes(raw)
    subprocess.run([str(packer), str(source), str(destination)], check=True)
    return destination.read_bytes()


def parse_qcpx(data: bytes) -> tuple[bytearray, bytes, bytes,
                                     list[tuple[bytes, bytes]], bytes]:
    if len(data) < TAIL_FILE_OFFSET + QCPZ_HEADER_SIZE + SONG_MAP_SIZE:
        raise ValueError("QCPX input is truncated")
    if data[:4] != b"KSSX" or data[4:12] != bytes.fromhex("00020100bb52df52"):
        raise ValueError("input is not the expected complete-page Quarth KSSX")
    tail = data[TAIL_FILE_OFFSET:]
    if tail[:4] != b"QCPX" or tail[4] != 1:
        raise ValueError("input has no QCPX v1 tail")
    page_count = tail[5]
    track_count = tail[6]
    if page_count == 0 or page_count > 32 or track_count != 19:
        raise ValueError("unexpected QCPX page/track count")
    engine_size = word(tail, 8)
    common_size = word(tail, 10)
    page_data_address = word(tail, 12)
    cursor = QCPZ_HEADER_SIZE + SONG_MAP_SIZE
    engine = bytes(tail[cursor:cursor + engine_size])
    cursor += engine_size
    common = bytes(tail[cursor:cursor + common_size])
    cursor += common_size
    if len(engine) != engine_size or len(common) != common_size:
        raise ValueError("truncated QCPX templates")
    pages: list[tuple[bytes, bytes]] = []
    for _ in range(page_count):
        if cursor + 4 > len(tail):
            raise ValueError("truncated QCPX page descriptor")
        data_size = word(tail, cursor)
        patch_count = word(tail, cursor + 2)
        cursor += 4
        patch_size = patch_count * 4
        patches = bytes(tail[cursor:cursor + patch_size])
        cursor += patch_size
        payload = bytes(tail[cursor:cursor + data_size])
        cursor += data_size
        if len(patches) != patch_size or len(payload) != data_size:
            raise ValueError("truncated QCPX page record")
        pages.append((patches, payload))

    extra_end = HEADER_SIZE + word(data, 0x10)
    if extra_end < TAIL_FILE_OFFSET + cursor or extra_end > len(data):
        raise ValueError("invalid QCPX extra-data size")
    info = data[extra_end:]
    fixed = bytearray(tail[:QCPZ_HEADER_SIZE + SONG_MAP_SIZE])
    fixed[:4] = b"QCPZ"
    return fixed, engine, common, pages, info


def append_staged_stream(tail: bytearray, stream: bytes) -> int:
    """Append a stream without crossing an absolute 16K staged-file page."""
    if len(stream) > STAGE_SIZE:
        raise ValueError("one compressed QCPZ stream exceeds a staging segment")
    absolute = TAIL_FILE_OFFSET + len(tail)
    in_segment = absolute & (STAGE_SIZE - 1)
    if in_segment + len(stream) > STAGE_SIZE:
        tail.extend(bytes(STAGE_SIZE - in_segment))
    offset = len(tail)
    tail.extend(stream)
    if offset > 0xFFFF:
        raise ValueError("QCPZ stream offset exceeds 16 bits")
    return offset


def build(source: Path, destination: Path, packer: Path) -> None:
    original = source.read_bytes()
    fixed, engine, common, pages, info = parse_qcpx(original)
    compressed_engine = compress(packer, "engine", engine)
    compressed_common = compress(packer, "common", common)
    compressed_pages = [
        compress(packer, f"page{index}", payload)
        for index, (_, payload) in enumerate(pages)
    ]

    page_count = fixed[5]
    directory_offset = len(fixed)
    tail = fixed + bytes(8 + page_count * PAGE_DESCRIPTOR_SIZE)

    patch_offsets: list[int] = []
    for patches, _ in pages:
        patch_offsets.append(len(tail))
        tail.extend(patches)

    engine_offset = append_staged_stream(tail, compressed_engine)
    common_offset = append_staged_stream(tail, compressed_common)
    page_offsets = [append_staged_stream(tail, stream) for stream in compressed_pages]

    put_word(tail, directory_offset + 0, len(compressed_engine))
    put_word(tail, directory_offset + 2, engine_offset)
    put_word(tail, directory_offset + 4, len(compressed_common))
    put_word(tail, directory_offset + 6, common_offset)
    for index, ((patches, payload), compressed_payload) in enumerate(
        zip(pages, compressed_pages)
    ):
        descriptor = directory_offset + 8 + index * PAGE_DESCRIPTOR_SIZE
        put_word(tail, descriptor + 0, len(payload))
        put_word(tail, descriptor + 2, len(patches) // 4)
        put_word(tail, descriptor + 4, len(compressed_payload))
        put_word(tail, descriptor + 6, page_offsets[index])
        put_word(tail, descriptor + 8, patch_offsets[index])

    header = bytearray(original[:HEADER_SIZE])
    put_word(header, 0x10, 1 + len(tail))
    result = bytes(header) + b"\xC9" + bytes(tail) + info
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    trackinfo = source.with_suffix(".trackinfo")
    if trackinfo.exists():
        destination.with_suffix(".trackinfo").write_bytes(trackinfo.read_bytes())

    print(f"wrote {destination}")
    print(f"  engine: {len(engine)} -> {len(compressed_engine)} bytes")
    print(f"  common: {len(common)} -> {len(compressed_common)} bytes")
    for index, ((_, payload), packed) in enumerate(zip(pages, compressed_pages)):
        print(f"  page {index}: {len(payload)} -> {len(packed)} bytes")
    print(f"  total: {len(result)} bytes (QCPX was {len(original)} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source", nargs="?", type=Path,
        default=Path("vigamup/extracted/quarth_16k_complete_page.ksp"),
    )
    parser.add_argument(
        "destination", nargs="?", type=Path,
        default=Path("vigamup/extracted/quarth_16k_complete_page_compressed.ksp"),
    )
    parser.add_argument("--zx0pack", type=Path, default=Path("build/zx0pack"))
    args = parser.parse_args()
    try:
        build(args.source, args.destination, args.zx0pack)
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
