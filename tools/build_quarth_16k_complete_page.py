#!/usr/bin/env python3
"""Build Quarth pages with one shared engine template.

The ordinary Quarth 16K repacker keeps the engine in the initial load image
and puts music in two mapper banks.  That is not suitable for the MSX player
layout where the active engine and its music must occupy the same mapper page.

QCPX stores the engine and common lookup block once.  Each logical page then
contains only its stream payload and a small list of descriptor patches.  The
player materializes complete 16K pages before starting the engine.  The pages
are mapped into MSX page 1 (4000H-7FFFH), leaving page 2 available for SCC.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct

from repack_quarth_banked import (
    BANK_SIZE,
    COMMON_END,
    COMMON_START,
    CORE_END,
    ENGINE_MUSIC_REFERENCES,
    HEADER_SIZE,
    LOAD_ADDRESS,
    TAIL_HELPER_END,
    TAIL_SOURCE_START,
    descriptor_address,
    image_slice,
    next_descriptor_address,
    patch_engine_bytes,
    read_trackinfo,
    source_word,
    u16,
)


QCPX = b"QCPX"
QCPX_VERSION = 1
PAGE_DATA_START = 0x65B5
WORKSPACE_SOURCE_START = 0xC400
WORKSPACE_SOURCE_END = 0xC68C
WORKSPACE_DESTINATION = 0x7D00

# These apparent words cross instruction boundaries in the exact Quarth
# engine image and must not be treated as absolute address operands.
WORKSPACE_FALSE_OPERANDS = {
    0x0538, 0x05D4, 0x05DB, 0x05E2, 0x05E9,
    0x09F3, 0x0A11,
    0x119D, 0x11A6, 0x11AF, 0x11B8, 0x11C1, 0x11CA, 0x11D3,
}
ABSOLUTE_WORD_OPCODES = {0x01, 0x11, 0x21, 0x22, 0x2A, 0x31, 0x32, 0x3A}

# Each range keeps every selected track's streams together.  The last two
# pages split the original second bank; no F9/FC/FD stream target crosses one
# of these boundaries.
PAGE_LAYOUT = (
    ((3, 4, 5, 6, 7, 8), (0x6573, 0x7320)),
    ((9, 10, 11), (0x7320, 0x8386)),
    ((12, 13, 14), (0x8386, 0x906D)),
    ((15, 16, 17, 18), (0x906D, 0xA200)),
    ((19, 20, 21), (0xA200, 0xAF09)),
)

A0F0_START = 0xA0F0
A0F0_END = 0xA270
VOLUME_TABLE_ADDRESS = 0x642A
VOLUME_TABLE_POINTERS = (
    0xA0F0,
    0x643A,
    0x645D,
    0x645E,
    0x646F,
    0x6483,
    0x649F,
    0x64BE,
)
WAVE_POINTER_TABLES = (
    (0x5482, 12),
    (0x549A, 12),
    (0x54B2, 12),
    (0x54CA, 12),
)


def put_u16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def patch_workspace_references(engine: bytearray) -> None:
    """Move Quarth's channel/control state from page 3 into page 1."""
    patched = 0
    for offset in range(1, len(engine) - 1):
        source = u16(engine, offset)
        if not WORKSPACE_SOURCE_START <= source < WORKSPACE_SOURCE_END:
            continue
        if engine[offset - 1] not in ABSOLUTE_WORD_OPCODES:
            continue
        if offset in WORKSPACE_FALSE_OPERANDS:
            continue
        put_u16(
            engine,
            offset,
            WORKSPACE_DESTINATION + source - WORKSPACE_SOURCE_START,
        )
        patched += 1
    if patched != 232:
        raise AssertionError(f"unexpected Quarth workspace patch count: {patched}")


def remove_legacy_scc_slot_switches(engine: bytearray) -> None:
    """Page 2 is permanently SCC; remove the old KSS-VM A8 slot shadows."""
    for address, expected in (
        (0x41C4, bytes.fromhex("3A FD FF D3 A8")),
        (0x41D2, bytes.fromhex("3A FE FF D3 A8")),
        (0x51DE, bytes.fromhex("3A FD FF D3 A8")),
        (0x51E5, bytes.fromhex("3A FE FF D3 A8")),
        (0x527D, bytes.fromhex("3A FD FF D3 A8")),
        (0x5294, bytes.fromhex("3A FE FF D3 A8")),
    ):
        offset = address - LOAD_ADDRESS
        actual = bytes(engine[offset:offset + len(expected)])
        if actual != expected:
            raise ValueError(
                f"unexpected SCC slot sequence at 0x{address:04X}: {actual.hex()}"
            )
        engine[offset:offset + len(expected)] = bytes(len(expected))


def complete_tail(image: bytes) -> bytes:
    """Keep the established 52BB/52DF entry addresses, without bank writes."""
    helper = image_slice(image, TAIL_SOURCE_START, TAIL_HELPER_END)
    # The old wrapper selected a physical/logical music bank.  The complete
    # page player selects the page before INIT, so this wrapper only enters
    # the original init and selector.  Keep the old offsets for compatibility.
    # ENGINE_INIT receives the original Quarth song ID in A. The reset entry
    # at 4000H changes A, so preserve it for the selector at 4003H.
    init = bytes((
        0xF5,                   # PUSH AF
        0xCD, 0x00, 0x40,       # CALL 4000H
        0xF1,                   # POP AF
        0xCD, 0x03, 0x40,       # CALL 4003H
        0xC9,                   # RET
    )) + bytes(10)
    selector = bytes((0xC9,)) + bytes(16)
    play = bytes((0xCD, 0x06, 0x40, 0xC9))
    stop = bytes((0xC9,))
    tail = helper + init + selector + play + stop
    if len(tail) != 0x40:
        raise AssertionError("unexpected complete-page tail size")
    return tail


def common_address(address: int, common_start: int) -> int:
    if not COMMON_START <= address < COMMON_END:
        raise ValueError(f"0x{address:04X} is not in common data")
    return common_start + address - COMMON_START


def patch_common_word(common: bytearray, address: int, value: int) -> None:
    offset = address - COMMON_START
    if offset < 0 or offset + 2 > len(common):
        raise ValueError(f"common word outside block: 0x{address:04X}")
    put_u16(common, offset, value)


def make_common(image: bytes, track_ids: tuple[int, ...], common_start: int) -> bytearray:
    common = bytearray(image_slice(image, COMMON_START, COMMON_END))

    for track_id in track_ids:
        entry = COMMON_START + 2 * track_id
        old = source_word(image, entry)
        patch_common_word(common, entry, common_address(old, common_start))

    for table_address, entry_count in WAVE_POINTER_TABLES:
        for index in range(entry_count):
            address = table_address + 2 * index
            old = source_word(image, address)
            patch_common_word(common, address, common_address(old, common_start))

    for index in range(0x5A):
        address = 0x5838 + 2 * index
        old = source_word(image, address)
        patch_common_word(common, address, common_address(old, common_start))

    if source_word(image, 0x64C5) != 0xFFC1:
        raise ValueError("unexpected 64C5H table sentinel")
    for index in range(1, 13):
        address = 0x64C5 + 2 * index
        old = source_word(image, address)
        patch_common_word(common, address, common_address(old, common_start))

    # Entries 1..7 are common data. Entry 0 points to A0F0H and is patched
    # separately for every materialized page.
    for index, expected in enumerate(VOLUME_TABLE_POINTERS[1:], 1):
        address = VOLUME_TABLE_ADDRESS + 2 * index
        old = source_word(image, address)
        if old != expected:
            raise ValueError(f"unexpected volume pointer {index}: 0x{old:04X}")
        patch_common_word(common, address, common_address(old, common_start))
    return common


def relocate_stream(
    image: bytes,
    source_start: int,
    source_end: int,
    destination_start: int,
) -> bytearray:
    data = bytearray(image_slice(image, source_start, source_end))
    for address in range(source_start, source_end - 2):
        opcode = image[address - LOAD_ADDRESS]
        if opcode not in (0xF9, 0xFC, 0xFD):
            continue
        target = source_word(image, address + 1)
        if not source_start <= target < source_end:
            continue
        put_u16(data, address - source_start + 1,
                destination_start + target - source_start)
    return data


def page_payload(
    image: bytes,
    track_ids: tuple[int, ...],
    source_start: int,
    source_end: int,
    common_start: int,
) -> tuple[bytes, list[tuple[int, int]]]:
    stream_destination = PAGE_DATA_START
    stream = relocate_stream(image, source_start, source_end, stream_destination)
    a0f0_destination = stream_destination + len(stream)
    stream += image_slice(image, A0F0_START, A0F0_END)

    def relocate_pointer(address: int) -> int:
        if source_start <= address < source_end:
            return stream_destination + address - source_start
        if A0F0_START <= address < A0F0_END:
            return a0f0_destination + address - A0F0_START
        if COMMON_START <= address < COMMON_END:
            return common_address(address, common_start)
        raise ValueError(f"page pointer 0x{address:04X} is not packed")

    patches: list[tuple[int, int]] = []
    patches.append((VOLUME_TABLE_ADDRESS - COMMON_START, a0f0_destination))
    for track_id in track_ids:
        descriptor = descriptor_address(image, track_id)
        end = next_descriptor_address(image, track_id)
        for address in range(descriptor + 2, end, 2):
            patches.append((address - COMMON_START,
                            relocate_pointer(source_word(image, address))))
    return bytes(stream), patches


def info_block(trackinfo: Path) -> bytes:
    rows = []
    for line in trackinfo.read_text(encoding="utf-8").splitlines():
        fields = line.split(",", 4)
        if len(fields) < 3:
            continue
        track_id = int(fields[0].strip())
        title = fields[1].strip()
        seconds = max(1, int(fields[2].strip()))
        fade = max(0, int(fields[3].strip() or 0)) if len(fields) >= 4 else 0
        loop = len(fields) == 5 and fields[4].strip().lower().startswith("y")
        rows.append((track_id, title, seconds, fade, loop))
    titles = [f"Quarth [#{track_id}] - {title}" for track_id, title, *_ in rows]
    result = bytearray(0x10 + sum(10 + len(t.encode()) + 1 for t in titles))
    result[:4] = b"INFO"
    put_u16(result, 8, len(rows))
    offset = 0x10
    for song, ((track_id, _, seconds, fade, loop), title) in enumerate(zip(rows, titles)):
        result[offset] = song
        result[offset + 1] = 1 if loop else 0
        put_u16(result, offset + 2, seconds * 1000 & 0xFFFF)
        put_u16(result, offset + 4, seconds * 1000 >> 16)
        put_u16(result, offset + 6, fade * 1000 & 0xFFFF)
        put_u16(result, offset + 8, fade * 1000 >> 16)
        offset += 10
        raw = title.encode() + b"\0"
        result[offset:offset + len(raw)] = raw
        offset += len(raw)
    put_u16(result, 4, len(result) - 0x10)
    return bytes(result)


def build(source: Path, destination: Path, trackinfo: Path, engine_path: Path) -> None:
    raw = source.read_bytes()
    if raw[:4] != b"KSCC":
        raise ValueError("source must be the original Quarth KSCC")
    if (u16(raw, 4), u16(raw, 6), u16(raw, 8), u16(raw, 10)) != (
        0x4000, 0x7824, 0xB817, 0xB820
    ):
        raise ValueError("source does not match the original Quarth layout")
    image = raw[HEADER_SIZE:HEADER_SIZE + u16(raw, 6)]
    extracted_engine = engine_path.read_bytes()
    expected_engine = image_slice(image, LOAD_ADDRESS, CORE_END) + image_slice(
        image, TAIL_SOURCE_START, 0xB824
    )
    if extracted_engine != expected_engine:
        raise ValueError(f"{engine_path} does not match the original Quarth engine layout")
    track_ids = read_trackinfo(trackinfo)
    expected = {track_id for tracks, _ in PAGE_LAYOUT for track_id in tracks}
    if set(track_ids) != expected:
        raise ValueError("trackinfo does not contain exactly the required Quarth IDs")

    core = bytearray(image_slice(image, LOAD_ADDRESS, CORE_END))
    tail = complete_tail(image)
    engine_size = len(core) + len(tail)
    common_start = LOAD_ADDRESS + engine_size
    if common_start != 0x52E4:
        raise AssertionError(f"unexpected common start 0x{common_start:04X}")
    common = make_common(image, track_ids, common_start)

    music_targets = {
        0x52A2: common_address(0x52A2, common_start),
        0x5482: common_address(0x5482, common_start),
        0x549A: common_address(0x549A, common_start),
        0x54B2: common_address(0x54B2, common_start),
        0x54CA: common_address(0x54CA, common_start),
        0x64C5: common_address(0x64C5, common_start),
        0x642A: common_address(0x642A, common_start),
        0x5838: common_address(0x5838, common_start),
        CORE_END: CORE_END,
    }
    for address, expected_bytes, _ in ENGINE_MUSIC_REFERENCES:
        old_target = u16(expected_bytes, 1)
        replacement = expected_bytes[:1] + struct.pack("<H", music_targets[old_target])
        patch_engine_bytes(core, address, expected_bytes, replacement)
    patch_engine_bytes(core, 0x4F3F, bytes.fromhex("CD 00 B8"), bytes((0xCD, 0xA4, 0x52)))
    patch_workspace_references(core)
    remove_legacy_scc_slot_switches(core)

    engine = bytes(core) + tail
    pages: list[tuple[bytes, list[tuple[int, int]], tuple[int, ...]]] = []
    for tracks, (start, end) in PAGE_LAYOUT:
        data, patches = page_payload(image, tracks, start, end, common_start)
        if engine_size + len(common) + len(data) > BANK_SIZE:
            raise ValueError(f"page {tracks} exceeds 16K")
        if PAGE_DATA_START + len(data) > WORKSPACE_DESTINATION:
            raise ValueError(f"page {tracks} overlaps the relocated Quarth workspace")
        pages.append((data, patches, tracks))

    song_to_original = [0xFF] * 256
    song_to_page = [0xFF] * 256
    for song, original in enumerate(track_ids):
        song_to_original[song] = original
        song_to_page[song] = next(index for index, (_, _, tracks) in enumerate(pages) if original in tracks)

    tail = bytearray()
    tail += QCPX
    tail += bytes((QCPX_VERSION, len(pages), len(track_ids), 0))
    tail += struct.pack("<HHHH", engine_size, len(common), PAGE_DATA_START, 0)
    tail += bytes(song_to_original) + bytes(song_to_page)
    tail += engine + common
    for data, patches, _ in pages:
        tail += struct.pack("<HH", len(data), len(patches))
        for offset, value in patches:
            tail += struct.pack("<HH", offset, value)
        tail += data

    load_image = b"\xC9"
    info = info_block(trackinfo)
    header = bytearray(0x20)
    header[:4] = b"KSSX"
    put_u16(header, 4, 0x0200)
    put_u16(header, 6, len(load_image))
    put_u16(header, 8, 0x52BB)
    put_u16(header, 10, 0x52DF)
    header[0x0C] = 0
    header[0x0D] = len(pages)
    header[0x0E] = 0x10
    header[0x0F] = 0
    put_u16(header, 0x10, len(load_image) + len(tail))
    put_u16(header, 0x18, 0)
    put_u16(header, 0x1A, len(track_ids) - 1)

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(bytes(header) + load_image + bytes(tail) + info)
    destination.with_suffix(".trackinfo").write_text(
        "\n".join(f"{index},{line.split(',', 1)[1]}" for index, line in enumerate(trackinfo.read_text().splitlines())) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {destination}")
    print(f"  shared engine: {len(engine)} bytes")
    print(f"  shared common: {len(common)} bytes")
    print(f"  pages: {len(pages)} x 16K, page 1 window at 4000H")
    for index, (data, patches, tracks) in enumerate(pages):
        print(f"  page {index}: tracks {tracks}, music {len(data)} bytes, patches {len(patches)}")
    print(f"  total: {destination.stat().st_size} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=Path("vigamup/quarth.kss"))
    parser.add_argument("destination", nargs="?", type=Path,
                        default=Path("vigamup/extracted/quarth_16k_complete_page.ksp"))
    parser.add_argument("--trackinfo", type=Path, default=Path("vigamup/quarth.trackinfo"))
    parser.add_argument("--engine", type=Path,
                        default=Path("vigamup/extracted/quarth.engine"),
                        help="address-preserving extracted engine to validate")
    args = parser.parse_args()
    try:
        build(args.source, args.destination, args.trackinfo, args.engine)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
