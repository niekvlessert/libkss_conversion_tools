#!/usr/bin/env python3
"""ZX0-compress the Salamander SCPX complete-page container as SCPZ."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 0x20
TAIL_FILE_OFFSET = 0x21
STAGE_SIZE = 0x4000
FIXED_SIZE = 16 + 512
PAGE_DESCRIPTOR_SIZE = 10


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def compress(packer: Path, name: str, raw: bytes) -> bytes:
    work = ROOT / "tmp" / "salamander-scpz-build"
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"{name}.raw"
    destination = work / f"{name}.zx0"
    source.write_bytes(raw)
    subprocess.run([str(packer), str(source), str(destination)], check=True)
    return destination.read_bytes()


def append_staged_stream(tail: bytearray, stream: bytes) -> int:
    if len(stream) > STAGE_SIZE:
        raise ValueError("one SCPZ stream exceeds a 16K staging segment")
    absolute = TAIL_FILE_OFFSET + len(tail)
    in_segment = absolute & (STAGE_SIZE - 1)
    if in_segment + len(stream) > STAGE_SIZE:
        tail.extend(bytes(STAGE_SIZE - in_segment))
    offset = len(tail)
    tail.extend(stream)
    if offset > 0xFFFF:
        raise ValueError("SCPZ stream offset exceeds 16 bits")
    return offset


def parse_scpx(data: bytes) -> tuple[bytearray, bytes,
                                      list[tuple[bytes, bytes]], bytes]:
    if len(data) < TAIL_FILE_OFFSET + FIXED_SIZE or data[:4] != b"KSSX":
        raise ValueError("SCPX input is truncated or not KSSX")
    tail = data[TAIL_FILE_OFFSET:]
    if tail[:4] != b"SCPX" or tail[4] != 1 or tail[5] != 14 or tail[6] != 14:
        raise ValueError("input has no supported Salamander SCPX tail")
    template_size = word(tail, 8)
    if template_size != 0x4000:
        raise ValueError("unexpected SCPX template size")
    cursor = FIXED_SIZE
    template = bytes(tail[cursor:cursor + template_size])
    cursor += template_size
    pages = []
    for _ in range(tail[5]):
        if cursor + 4 > len(tail):
            raise ValueError("truncated SCPX record")
        data_size = word(tail, cursor)
        patch_count = word(tail, cursor + 2)
        cursor += 4
        patches = bytes(tail[cursor:cursor + patch_count * 4])
        cursor += patch_count * 4
        payload = bytes(tail[cursor:cursor + data_size])
        cursor += data_size
        if len(patches) != patch_count * 4 or len(payload) != data_size:
            raise ValueError("truncated SCPX page")
        pages.append((patches, payload))
    extra_end = HEADER_SIZE + word(data, 0x10)
    if extra_end < TAIL_FILE_OFFSET + cursor or extra_end > len(data):
        raise ValueError("invalid SCPX extra-data size")
    fixed = bytearray(tail[:FIXED_SIZE])
    fixed[:4] = b"SCPZ"
    return fixed, template, pages, data[extra_end:]


def build(source: Path, destination: Path, packer: Path) -> None:
    original = source.read_bytes()
    fixed, template, pages, info = parse_scpx(original)
    packed_template = compress(packer, "template", template)
    packed_pages = [
        compress(packer, f"page{index}", payload)
        for index, (_, payload) in enumerate(pages)
    ]
    directory = len(fixed)
    tail = fixed + bytes(4 + len(pages) * PAGE_DESCRIPTOR_SIZE)
    patch_offsets = []
    for patches, _ in pages:
        patch_offsets.append(len(tail))
        tail.extend(patches)
    template_offset = append_staged_stream(tail, packed_template)
    page_offsets = [append_staged_stream(tail, packed) for packed in packed_pages]
    put_word(tail, directory, len(packed_template))
    put_word(tail, directory + 2, template_offset)
    for index, ((patches, payload), packed) in enumerate(zip(pages, packed_pages)):
        descriptor = directory + 4 + index * PAGE_DESCRIPTOR_SIZE
        put_word(tail, descriptor, len(payload))
        put_word(tail, descriptor + 2, len(patches) // 4)
        put_word(tail, descriptor + 4, len(packed))
        put_word(tail, descriptor + 6, page_offsets[index])
        put_word(tail, descriptor + 8, patch_offsets[index])

    header = bytearray(original[:HEADER_SIZE])
    put_word(header, 0x10, 1 + len(tail))
    result = bytes(header) + b"\xC9" + bytes(tail) + info
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    source_trackinfo = source.with_suffix(".trackinfo")
    if source_trackinfo.exists():
        destination.with_suffix(".trackinfo").write_bytes(source_trackinfo.read_bytes())
    print(f"wrote {destination}")
    print(f"  template: {len(template)} -> {len(packed_template)} bytes")
    for index, ((_, payload), packed) in enumerate(zip(pages, packed_pages)):
        print(f"  page {index}: {len(payload)} -> {len(packed)} bytes")
    print(f"  total: {len(result)} bytes (SCPX was {len(original)} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path,
                        default=Path("vigamup/extracted/salamander_complete_page.ksp"))
    parser.add_argument("destination", nargs="?", type=Path,
                        default=Path("vigamup/extracted/salamander_complete_page_compressed.ksp"))
    parser.add_argument("--zx0pack", type=Path, default=Path("build/zx0pack"))
    args = parser.parse_args()
    try:
        build(args.source, args.destination, args.zx0pack)
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
