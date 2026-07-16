#!/usr/bin/env python3
"""Build dynamically materialized Salamander complete pages.

The original KSCC places its driver, song data and wrappers in one
6000H-C046H image and keeps writable driver state at E000H-E1EAH. SCPX stores
one sparse 16K page-1 template plus one relocated payload per required song.
The player selects a logical song page; physical mapper allocation is never
encoded in the engine.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


HEADER_SIZE = 0x10
LOAD_ADDRESS = 0x6000
CORE_END = 0x73C8
COMMON_END = 0x74B0
WRAPPER_SOURCE = 0xC000
WRAPPER_END = 0xC047
WRAPPER_DESTINATION = 0x5F80
INIT_ADDRESS = WRAPPER_DESTINATION + 0x17
PLAY_ADDRESS = WRAPPER_DESTINATION + 0x43
PSG_GATEWAY = WRAPPER_DESTINATION + (WRAPPER_END - WRAPPER_SOURCE)
WORK_SOURCE = 0xE000
WORK_END = 0xE1EB
WORK_DESTINATION = 0x7E00
PAGE_ADDRESS = 0x4000
PAGE_SIZE = 0x4000
PAYLOAD_ADDRESS = 0x4000
SONG_TABLE = 0x663E

TRACK_RANGES = {
    24: (0x9BB3, 0x9EB4),
    26: (0x8310, 0x884E),
    27: (0x884E, 0x8C3D),
    28: (0x8C3D, 0x900B),
    29: (0x900B, 0x9441),
    30: (0x9441, 0x9742),
    31: (0x9742, 0x9BB3),
    32: (0xA079, 0xA474),
    33: (0x9EB4, 0xA079),
    38: (0xA62A, 0xAB5B),
    39: (0xAB5B, 0xAE62),
    40: (0xAE62, 0xB37D),
    41: (0xB37D, 0xB934),
    42: (0xA474, 0xA62A),
}

ABSOLUTE_WORD_OPCODES = {0x01, 0x11, 0x21, 0x22, 0x2A, 0x31, 0x32, 0x3A}
ED_WORD_OPCODES = {0x43, 0x4B, 0x53, 0x5B, 0x63, 0x6B, 0x73, 0x7B}


def word(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def image_slice(image: bytes, start: int, end: int) -> bytes:
    if start < LOAD_ADDRESS or end > LOAD_ADDRESS + len(image) or end < start:
        raise ValueError(f"invalid source range {start:04X}-{end:04X}")
    return image[start - LOAD_ADDRESS:end - LOAD_ADDRESS]


def read_trackinfo(path: Path) -> list[tuple[int, str, int, int, bool]]:
    result = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split(",", 4)
        if len(fields) < 3:
            continue
        track = int(fields[0].strip())
        seconds = max(1, int(fields[2].strip()))
        fade = int(fields[3].strip() or 0) if len(fields) >= 4 else 0
        loop = len(fields) == 5 and fields[4].strip().lower().startswith("y")
        result.append((track, fields[1].strip(), seconds, max(0, fade), loop))
    if [row[0] for row in result] != list(TRACK_RANGES):
        raise ValueError("Salamander trackinfo order/range does not match the analyzed set")
    return result


def info_block(rows: list[tuple[int, str, int, int, bool]]) -> bytes:
    titles = [f"Salamander [#{track}] - {title}" for track, title, *_ in rows]
    result = bytearray(0x10 + sum(10 + len(title.encode()) + 1 for title in titles))
    result[:4] = b"INFO"
    put_word(result, 8, len(rows))
    cursor = 0x10
    for song, ((_, _, seconds, fade, loop), title) in enumerate(zip(rows, titles)):
        result[cursor] = song
        result[cursor + 1] = int(loop)
        struct.pack_into("<II", result, cursor + 2, seconds * 1000, fade * 1000)
        cursor += 10
        encoded = title.encode() + b"\0"
        result[cursor:cursor + len(encoded)] = encoded
        cursor += len(encoded)
    put_word(result, 4, len(result) - 0x10)
    return bytes(result)


def relocate_workspace(code: bytearray, source_address: int) -> int:
    patched = 0
    for operand in range(1, len(code) - 1):
        value = word(code, operand)
        if not WORK_SOURCE <= value < WORK_END:
            continue
        normal = code[operand - 1] in ABSOLUTE_WORD_OPCODES
        indexed = operand >= 2 and code[operand - 2] in (0xDD, 0xFD) and code[operand - 1] == 0x21
        extended = operand >= 2 and code[operand - 2] == 0xED and code[operand - 1] in ED_WORD_OPCODES
        if not (normal or indexed or extended):
            continue
        put_word(code, operand, WORK_DESTINATION + value - WORK_SOURCE)
        patched += 1
    return patched


def remove_scc_slot_switches(core: bytearray) -> None:
    for address, expected in (
        (0x65C2, bytes.fromhex("3A FD FF D3 A8")),
        (0x65CB, bytes.fromhex("3A FE FF D3 A8")),
        (0x6604, bytes.fromhex("3A FD FF D3 A8")),
        (0x6620, bytes.fromhex("3A FE FF D3 A8")),
        (0x7348, bytes.fromhex("3A FD FF D3 A8")),
        (0x734F, bytes.fromhex("3A FE FF D3 A8")),
        (0x73A9, bytes.fromhex("3A FD FF D3 A8")),
        (0x73C2, bytes.fromhex("3A FE FF D3 A8")),
    ):
        offset = address - LOAD_ADDRESS
        actual = bytes(core[offset:offset + 5])
        if actual != expected:
            raise ValueError(f"unexpected SCC slot sequence at {address:04X}: {actual.hex()}")
        core[offset:offset + 5] = bytes(5)


def replace_bios_psg_calls(core: bytearray, wrapper: bytearray) -> int:
    """Route BIOS WRTPSG calls through a page-1-resident direct gateway.

    MSX-DOS2 keeps page 0 as DOS/RAM, so a direct CALL/JP 0093H does not
    necessarily enter the BIOS slot. The gateway preserves Salamander's
    A=register, E=value convention without touching page 0 or any slot state.
    """
    replacement = struct.pack("<H", PSG_GATEWAY)
    patched = 0
    for block in (core, wrapper):
        for offset in range(len(block) - 2):
            if block[offset] in (0xC3, 0xCD) and block[offset + 1:offset + 3] == b"\x93\x00":
                block[offset + 1:offset + 3] = replacement
                patched += 1
    if patched != 10:
        raise ValueError(f"unexpected Salamander BIOS WRTPSG reference count: {patched}")
    return patched


def relocate_stream(raw: bytes, start: int, end: int) -> bytes:
    stream = bytearray(image_slice(raw, start, end))
    delta = PAYLOAD_ADDRESS - start
    patched = set()
    for offset, opcode in enumerate(stream):
        operand = None
        if opcode in (0xF9, 0xFD):
            operand = offset + 1
        elif opcode in (0xFB, 0xFC):
            operand = offset + 2
        if operand is None or operand + 2 > len(stream):
            continue
        target = word(stream, operand)
        if start <= target < end:
            put_word(stream, operand, target + delta)
            patched.add(operand)
    return bytes(stream)


def build(source: Path, destination: Path, trackinfo: Path, engine_file: Path) -> None:
    source_data = source.read_bytes()
    if source_data[:4] != b"KSCC" or struct.unpack_from("<HHHH", source_data, 4) != (
        0x6000, 0x6047, 0xC017, 0xC043
    ):
        raise ValueError("source is not the expected original Salamander KSCC")
    image = source_data[HEADER_SIZE:HEADER_SIZE + 0x6047]
    extracted = engine_file.read_bytes()
    expected = image_slice(image, 0x6000, CORE_END) + image_slice(image, WRAPPER_SOURCE, WRAPPER_END)
    if extracted != expected:
        raise ValueError("extracted Salamander engine does not match source segments")
    rows = read_trackinfo(trackinfo)

    core = bytearray(image_slice(image, 0x6000, CORE_END))
    wrapper = bytearray(image_slice(image, WRAPPER_SOURCE, WRAPPER_END))
    remove_scc_slot_switches(core)
    psg_patches = replace_bios_psg_calls(core, wrapper)
    call_offset = 0x708A - LOAD_ADDRESS
    if core[call_offset:call_offset + 3] != bytes.fromhex("CD 00 C0"):
        raise ValueError("unexpected Salamander C000H helper call")
    core[call_offset:call_offset + 3] = bytes((0xCD, WRAPPER_DESTINATION & 0xFF,
                                               WRAPPER_DESTINATION >> 8))

    workspace_patches = 0
    for start, end in ((0x6472, 0x663E), (0x6978, CORE_END)):
        part = core[start - LOAD_ADDRESS:end - LOAD_ADDRESS]
        workspace_patches += relocate_workspace(part, start)
        core[start - LOAD_ADDRESS:end - LOAD_ADDRESS] = part
    workspace_patches += relocate_workspace(wrapper, WRAPPER_SOURCE)
    if workspace_patches < 150:
        raise ValueError(f"too few Salamander workspace references patched: {workspace_patches}")

    template = bytearray(PAGE_SIZE)
    template[0x6000 - PAGE_ADDRESS:CORE_END - PAGE_ADDRESS] = core
    template[CORE_END - PAGE_ADDRESS:COMMON_END - PAGE_ADDRESS] = image_slice(
        image, CORE_END, COMMON_END
    )
    template[WRAPPER_DESTINATION - PAGE_ADDRESS:
             WRAPPER_DESTINATION - PAGE_ADDRESS + len(wrapper)] = wrapper
    template[PSG_GATEWAY - PAGE_ADDRESS:PSG_GATEWAY - PAGE_ADDRESS + 6] = bytes.fromhex(
        "D3 A0 7B D3 A1 C9"
    )

    pages = []
    for track, *_ in rows:
        start, end = TRACK_RANGES[track]
        payload = relocate_stream(image, start, end)
        descriptor = word(image, SONG_TABLE - LOAD_ADDRESS + 2 * track)
        patches = []
        for address in range(descriptor + 2, descriptor + 18, 2):
            value = word(image, address - LOAD_ADDRESS)
            if start <= value < end:
                patches.append((address - PAGE_ADDRESS, PAYLOAD_ADDRESS + value - start))
        if not patches:
            raise ValueError(f"track {track} has no relocated descriptor pointers")
        pages.append((track, payload, patches))

    original_map = bytes([row[0] for row in rows] + [0xFF] * (256 - len(rows)))
    page_map = bytes(list(range(len(rows))) + [0xFF] * (256 - len(rows)))
    tail = bytearray(b"SCPX")
    tail += bytes((1, len(pages), len(rows), 0))
    tail += struct.pack("<HHHH", PAGE_SIZE, PAYLOAD_ADDRESS, WORK_DESTINATION, workspace_patches)
    tail += original_map + page_map
    tail += template
    for _, payload, patches in pages:
        tail += struct.pack("<HH", len(payload), len(patches))
        for offset, value in patches:
            tail += struct.pack("<HH", offset, value)
        tail += payload

    header = bytearray(0x20)
    header[:4] = b"KSSX"
    struct.pack_into("<HHHH", header, 4, 0x0200, 1, INIT_ADDRESS, PLAY_ADDRESS)
    header[0x0C] = 0
    header[0x0D] = len(pages)
    header[0x0E] = 0x10
    header[0x0F] = 0
    put_word(header, 0x10, 1 + len(tail))
    put_word(header, 0x18, 0)
    put_word(header, 0x1A, len(rows) - 1)
    result = bytes(header) + b"\xC9" + bytes(tail) + info_block(rows)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    destination.with_suffix(".trackinfo").write_text(
        "\n".join(f"{index},{line.split(',', 1)[1]}" for index, line in enumerate(
            trackinfo.read_text(encoding="utf-8").splitlines()
        )) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {destination}")
    print(f"  workspace references relocated: {workspace_patches}")
    print(f"  BIOS WRTPSG references redirected: {psg_patches}")
    print(f"  shared sparse template: {len(template)} bytes")
    for index, (track, payload, patches) in enumerate(pages):
        print(f"  page {index}: track {track}, payload {len(payload)}, patches {len(patches)}")
    print(f"  total: {len(result)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=Path("vigamup/salamander.kss"))
    parser.add_argument("destination", nargs="?", type=Path,
                        default=Path("vigamup/extracted/salamander_complete_page.kss"))
    parser.add_argument("--trackinfo", type=Path, default=Path("vigamup/salamander.trackinfo"))
    parser.add_argument("--engine", type=Path, default=Path("vigamup/extracted/salamander.engine"))
    args = parser.parse_args()
    try:
        build(args.source, args.destination, args.trackinfo, args.engine)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
