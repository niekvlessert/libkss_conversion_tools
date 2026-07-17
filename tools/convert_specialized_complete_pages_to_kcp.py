#!/usr/bin/env python3
"""Convert proven Quarth QCPX or Salamander SCPX pages to generic KCP.

The specialized source files remain untouched. Their complete 16K logical
pages are materialized exactly once, then encoded as a generic shared template
plus sparse overlays in KCPX or ZX0-compressed KCPZ form.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct

from build_konami_complete_pages import (
    FIXED_SIZE, HEADER_SIZE, PAGE_SIZE, common_template, compress,
    overlay_from_template, put_word,
)


ROOT = Path(__file__).resolve().parents[1]
TAIL_OFFSET = HEADER_SIZE + 1
SPECIAL_FIXED_SIZE = 16 + 512
STAGE_SIZE = 0x4000


def word(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def split_container(path: Path) -> tuple[bytearray, bytes, bytes]:
    data = path.read_bytes()
    if len(data) < TAIL_OFFSET + SPECIAL_FIXED_SIZE or data[:4] != b"KSSX":
        raise ValueError(f"{path} is not a supported KSSX container")
    extra_end = HEADER_SIZE + word(data, 0x10)
    if extra_end < TAIL_OFFSET + SPECIAL_FIXED_SIZE or extra_end > len(data):
        raise ValueError(f"{path} has an invalid extra-data length")
    return bytearray(data[:HEADER_SIZE]), data[TAIL_OFFSET:extra_end], data[extra_end:]


def materialize_quarth(tail: bytes) -> tuple[list[bytearray], bytes, bytes, int]:
    if tail[:5] != b"QCPX\x01":
        raise ValueError("Quarth source must be the proven uncompressed QCPX")
    page_count = tail[5]
    track_count = tail[6]
    engine_size = word(tail, 8)
    common_size = word(tail, 10)
    payload_address = word(tail, 12)
    if not page_count or track_count != 19 or not 0x4000 <= payload_address < 0x8000:
        raise ValueError("unexpected Quarth QCPX layout")
    original_map = tail[16:272]
    page_map = tail[272:528]
    cursor = SPECIAL_FIXED_SIZE
    engine = tail[cursor:cursor + engine_size]
    cursor += engine_size
    common = tail[cursor:cursor + common_size]
    cursor += common_size
    if len(engine) != engine_size or len(common) != common_size or \
            engine_size + common_size > PAGE_SIZE:
        raise ValueError("truncated Quarth template")

    pages = []
    for _ in range(page_count):
        if cursor + 4 > len(tail):
            raise ValueError("truncated Quarth page record")
        data_size = word(tail, cursor)
        patch_count = word(tail, cursor + 2)
        cursor += 4
        patch_bytes = patch_count * 4
        patches = tail[cursor:cursor + patch_bytes]
        cursor += patch_bytes
        payload = tail[cursor:cursor + data_size]
        cursor += data_size
        if len(patches) != patch_bytes or len(payload) != data_size:
            raise ValueError("truncated Quarth page payload")
        page = bytearray(PAGE_SIZE)
        page[:engine_size] = engine
        page[engine_size:engine_size + common_size] = common
        payload_offset = payload_address - 0x4000
        if payload_offset + data_size > PAGE_SIZE:
            raise ValueError("Quarth payload exceeds page 1")
        page[payload_offset:payload_offset + data_size] = payload
        for offset in range(0, patch_bytes, 4):
            target = engine_size + word(patches, offset)
            if target + 2 > PAGE_SIZE:
                raise ValueError("Quarth patch exceeds page 1")
            put_word(page, target, word(patches, offset + 2))
        pages.append(page)
    return pages, original_map, page_map, 0x7D00


def materialize_salamander(tail: bytes) -> tuple[list[bytearray], bytes, bytes, int]:
    if tail[:5] != b"SCPX\x01":
        raise ValueError("Salamander source must be the proven uncompressed SCPX")
    page_count = tail[5]
    track_count = tail[6]
    template_size = word(tail, 8)
    payload_address = word(tail, 10)
    workspace = word(tail, 12)
    if page_count != 14 or track_count != 14 or template_size != PAGE_SIZE or \
            payload_address != 0x4000:
        raise ValueError("unexpected Salamander SCPX layout")
    original_map = tail[16:272]
    page_map = tail[272:528]
    cursor = SPECIAL_FIXED_SIZE
    template = tail[cursor:cursor + template_size]
    cursor += template_size
    if len(template) != PAGE_SIZE:
        raise ValueError("truncated Salamander template")

    pages = []
    for _ in range(page_count):
        if cursor + 4 > len(tail):
            raise ValueError("truncated Salamander page record")
        data_size = word(tail, cursor)
        patch_count = word(tail, cursor + 2)
        cursor += 4
        patch_bytes = patch_count * 4
        patches = tail[cursor:cursor + patch_bytes]
        cursor += patch_bytes
        payload = tail[cursor:cursor + data_size]
        cursor += data_size
        if len(patches) != patch_bytes or len(payload) != data_size or data_size > PAGE_SIZE:
            raise ValueError("truncated Salamander page payload")
        page = bytearray(template)
        page[:data_size] = payload
        for offset in range(0, patch_bytes, 4):
            target = word(patches, offset)
            if target + 2 > PAGE_SIZE:
                raise ValueError("Salamander patch exceeds page 1")
            put_word(page, target, word(patches, offset + 2))
        pages.append(page)
    return pages, original_map, page_map, workspace


def write_generic(source: Path, destination: Path, compressed: bool,
                  variant: int, packer: Path) -> None:
    header, specialized_tail, info = split_container(source)
    if specialized_tail[:4] == b"QCPX":
        pages, original_map, page_map, workspace = materialize_quarth(specialized_tail)
    elif specialized_tail[:4] == b"SCPX":
        pages, original_map, page_map, workspace = materialize_salamander(specialized_tail)
    else:
        raise ValueError("only uncompressed QCPX and SCPX sources are accepted")

    track_count = specialized_tail[6]
    template = common_template(pages)
    overlays = [overlay_from_template(template, page) for page in pages]
    fixed = bytearray(b"KCPZ" if compressed else b"KCPX")
    fixed += bytes((1, len(pages), track_count, variant))
    fixed += struct.pack("<HHHH", PAGE_SIZE, 0x4000, workspace, 0)
    fixed += original_map + page_map
    if len(fixed) != FIXED_SIZE:
        raise AssertionError("unexpected generic KCP fixed-header size")

    stem = destination.stem
    if compressed:
        packed_template = compress(packer, f"{stem}-template", template)
        packed_overlays = [compress(packer, f"{stem}-overlay-{index}", overlay)
                           for index, overlay in enumerate(overlays)]
        tail = fixed + bytes(4 + len(pages) * 6)
        def append_staged(stream: bytes) -> int:
            absolute = TAIL_OFFSET + len(tail)
            in_segment = absolute & (STAGE_SIZE - 1)
            if in_segment + len(stream) > STAGE_SIZE:
                tail.extend(bytes(STAGE_SIZE - in_segment))
            offset = len(tail)
            if offset > 0xFFFF:
                raise ValueError("generic KCPZ stream offset exceeds 16 bits")
            tail.extend(stream)
            return offset

        put_word(tail, FIXED_SIZE, len(packed_template))
        put_word(tail, FIXED_SIZE + 2, append_staged(packed_template))
        for index, (raw, packed) in enumerate(zip(overlays, packed_overlays)):
            descriptor = FIXED_SIZE + 4 + index * 6
            put_word(tail, descriptor, len(raw))
            put_word(tail, descriptor + 2, len(packed))
            put_word(tail, descriptor + 4, append_staged(packed))
    else:
        tail = fixed + template
        for overlay in overlays:
            tail += struct.pack("<H", len(overlay)) + overlay

    header[0x0D] = len(pages)
    put_word(header, 0x10, 1 + len(tail))
    result = bytes(header) + b"\xC9" + bytes(tail) + info
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    source_trackinfo = source.with_suffix(".trackinfo")
    if source_trackinfo.exists():
        destination.with_suffix(".trackinfo").write_bytes(source_trackinfo.read_bytes())
    print(f"wrote {destination} ({len(result)} bytes, {track_count} tracks, "
          f"{len(pages)} pages)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--compressed", action="store_true")
    parser.add_argument("--variant", type=int, required=True)
    parser.add_argument("--zx0pack", type=Path, default=ROOT / "build" / "zx0pack")
    args = parser.parse_args()
    try:
        write_generic(args.source, args.destination, args.compressed,
                      args.variant, args.zx0pack)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
