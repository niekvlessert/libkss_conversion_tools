#!/usr/bin/env python3
"""Repack integrated Konami SCC KSS drivers as dynamic page-1 containers.

KCPX stores one shared 16K page-1 template and one sparse overlay per song.
KCPZ applies ZX0 to the template and overlays.  Logical song/page numbers are
kept in the file; a player chooses physical mapper segments at load time.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import subprocess
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 0x20
PAGE_ADDRESS = 0x4000
PAGE_SIZE = 0x4000
FIXED_SIZE = 528
WRAPPER_DEST = 0x5F80
PSG_GATEWAY = 0x5FD0
PAYLOAD_LIMIT = 0x7C00
ABSOLUTE_WORD_OPCODES = {0x01, 0x11, 0x21, 0x22, 0x2A, 0x31, 0x32, 0x3A,
                         0xC2, 0xC3, 0xC4, 0xCA, 0xCC, 0xCD, 0xD2, 0xD4,
                         0xDA, 0xDC, 0xE2, 0xE4, 0xEA, 0xEC, 0xF2, 0xF4,
                         0xFA, 0xFC}
ED_WORD_OPCODES = {0x43, 0x4B, 0x53, 0x5B, 0x63, 0x6B, 0x73, 0x7B}


@dataclass(frozen=True)
class Config:
    variant: int
    title: str
    load: int
    load_size: int
    init: int
    play: int
    song_table: int
    code_start: int
    engine_end: int
    wrapper_end: int | None
    work_start: int
    work_end: int
    work_dest: int
    patch_stream_commands: bool = True
    main_start: int | None = None
    psg_gateway: int = PSG_GATEWAY
    extra_maps: tuple[tuple[int, int, int], ...] = ()
    main_destination: int | None = None
    wrapper_start: int = 0xC000
    pack_overlays: bool = False
    exhaustive_operand_patch: bool = False


CONFIGS = {
    "contra": Config(1, "Contra", 0x6000, 0x4000, 0x6003, 0x6006,
                     0x6E12, 0x6000, 0x6E14, None, 0xC000, 0xC200, 0x7D00,
                     False),
    "kingsvalley2": Config(2, "King's Valley 2", 0x6000, 0x6024,
                           0xC017, 0xC020, 0x6F2C, 0x6000, 0x6F2C, 0xC024,
                           0xE000, 0xE200, 0x7D00),
    "nemesis2": Config(3, "Nemesis 2", 0x6000, 0x6047, 0xC017, 0xC043,
                       0x6612, 0x6480, 0x73C8, 0xC047, 0xE000, 0xE200, 0x7D00),
    "parodius": Config(4, "Parodius", 0x4000, 0x8024, 0xC017, 0xC020,
                       0x4268, 0x4000, 0x50EC, 0xC024, 0xE000, 0xE200, 0x7D00),
    "spacemanbow": Config(5, "Space Manbow", 0x2000, 0xA029, 0xC017,
                          0xC025, 0x36F8, 0x2000, 0x3600, 0xC029,
                          0xC600, 0xC900, 0x7C00),
}


def word(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value & 0xFFFF)


def read_trackinfo(path: Path) -> list[tuple[int, str, int, int, bool]]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split(",", 4)
        if len(fields) < 3 or not fields[0].strip().isdigit():
            continue
        fade = int(fields[3].strip() or 0) if len(fields) >= 4 else 0
        loop = len(fields) == 5 and fields[4].strip().lower().startswith("y")
        rows.append((int(fields[0]), fields[1].strip(),
                     max(1, int(fields[2])), max(0, fade), loop))
    if not rows:
        raise ValueError(f"no tracks in {path}")
    return rows


def read_trace_ranges(path: Path) -> dict[int, list[tuple[int, int]]]:
    result: dict[int, list[tuple[int, int]]] = {}
    current = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("track="):
            current = int(line[6:])
        elif line.startswith("loaded_data_read_ranges=") and current is not None:
            ranges = []
            value = line.split("=", 1)[1]
            if value != "none":
                for item in value.split(","):
                    start, end = item.split("-")
                    ranges.append((int(start, 16), int(end, 16)))
            result[current] = ranges
    return result


def image_slice(image: bytes, cfg: Config, start: int, end: int) -> bytes:
    if start < cfg.load or end > cfg.load + len(image) or end < start:
        raise ValueError(f"source range {start:04X}-{end:04X} is outside image")
    return image[start - cfg.load:end - cfg.load]


def merged_ranges(ranges: list[tuple[int, int]], gap: int = 0x20) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if end <= start:
            continue
        if merged and start <= merged[-1][1] + gap:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(a, b) for a, b in merged]


class AddressMap:
    def __init__(self) -> None:
        self.ranges: list[tuple[int, int, int]] = []

    def add(self, source: int, end: int, destination: int) -> None:
        self.ranges.append((source, end, destination))

    def get(self, address: int) -> int | None:
        for start, end, destination in self.ranges:
            if start <= address < end:
                return destination + address - start
        return None


def copy_range(page: bytearray, image: bytes, cfg: Config, amap: AddressMap,
               source: int, end: int, destination: int) -> None:
    payload = image_slice(image, cfg, source, end)
    offset = destination - PAGE_ADDRESS
    if offset < 0 or offset + len(payload) > PAGE_SIZE:
        raise ValueError("page copy is outside page 1")
    page[offset:offset + len(payload)] = payload
    amap.add(source, end, destination)


def disassemble_starts(raw: bytes, origin: int, name: str) -> list[int]:
    work = ROOT / "tmp" / "konami-complete-pages" / "disasm"
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"{name}.bin"
    output = work / f"{name}.asm"
    source.write_bytes(raw)
    subprocess.run(["z80dasm", "-a", "-g", hex(origin), "-o", str(output),
                    str(source)], check=True)
    starts = []
    for line in output.read_text(encoding="utf-8").splitlines():
        match = re.search(r";([0-9a-fA-F]{4})$", line)
        if match:
            starts.append(int(match.group(1), 16))
    return starts


def patch_code(page: bytearray, amap: AddressMap, source_address: int,
               destination: int, size: int, instruction_starts: list[int],
               psg_gateway: int = PSG_GATEWAY,
               exhaustive_operand_patch: bool = False) -> int:
    start = destination - PAGE_ADDRESS
    code = page[start:start + size]
    patched = 0

    # Original game slot switching is invalid in the DOS2 player. The player
    # establishes page 1 RAM and page 2 SCC before entering the engine.
    for signature in (bytes.fromhex("3A FD FF D3 A8"),
                      bytes.fromhex("3A FE FF D3 A8")):
        cursor = 0
        while True:
            found = code.find(signature, cursor)
            if found < 0:
                break
            # Keep LD A,(FFFD/FFFE) and replace the 12-cycle OUT with the
            # equally long, equally timed JR +0.  NOPing all five bytes made
            # playback phase drift by five cycles at exact track boundaries.
            code[found + 3:found + 5] = bytes.fromhex("18 00")
            cursor = found + len(signature)
            patched += 1

    for instruction in instruction_starts:
        offset = instruction - source_address
        if offset < 0 or offset >= len(code):
            continue
        opcode = code[offset]
        operand = None
        if opcode in ABSOLUTE_WORD_OPCODES:
            operand = offset + 1
        elif opcode in (0xDD, 0xFD) and offset + 1 < len(code) and \
                code[offset + 1] in ABSOLUTE_WORD_OPCODES:
            operand = offset + 2
            opcode = code[offset + 1]
        elif opcode == 0xED and offset + 1 < len(code) and \
                code[offset + 1] in ED_WORD_OPCODES:
            operand = offset + 2
            opcode = code[offset + 1]
        if operand is None or operand + 2 > len(code):
            continue
        value = word(code, operand)
        replacement = amap.get(value)
        # SCC register/wave RAM is physically fixed even when the original
        # KSS also happened to store song bytes at the same source addresses.
        # Code literals target the device; descriptor/stream pointers target
        # relocated data and are patched separately below.
        if value in (0x9000, 0xB000) or 0x9800 <= value <= 0x98FF:
            replacement = None
        if value == 0x0093 and replacement is None and opcode in (0xC3, 0xCD):
            replacement = psg_gateway
        if replacement is not None and replacement != value:
            put_word(code, operand, replacement)
            patched += 1

    # Linear disassembly can lose synchronization on embedded tables. Recover
    # only missed workspace operands here; the narrow mapped destination range
    # avoids the false code/data relocations of the former global byte scan.
    for operand in range(1, len(code) - 1):
        value = word(code, operand)
        replacement = amap.get(value)
        if replacement is None or replacement == value or \
                (replacement < PAYLOAD_LIMIT and
                 not exhaustive_operand_patch):
            continue
        opcode = code[operand - 1]
        normal = opcode in ABSOLUTE_WORD_OPCODES
        indexed = (operand >= 2 and code[operand - 2] in (0xDD, 0xFD)
                   and opcode in ABSOLUTE_WORD_OPCODES)
        extended = (operand >= 2 and code[operand - 2] == 0xED
                    and opcode in ED_WORD_OPCODES)
        if normal or indexed or extended:
            put_word(code, operand, replacement)
            patched += 1
    page[start:start + size] = code
    return patched


def descriptor_address(image: bytes, cfg: Config, track: int) -> int:
    return word(image, cfg.song_table - cfg.load + 2 * track)


def allocate_overlay_ranges(cfg: Config, ranges: list[tuple[int, int]],
                            main_destination: int,
                            main_destination_end: int) -> list[tuple[int, int, int]]:
    occupied = [(main_destination, main_destination_end),
                (cfg.psg_gateway, cfg.psg_gateway + 8),
                (cfg.work_dest, cfg.work_dest + cfg.work_end - cfg.work_start)]
    if cfg.wrapper_end is not None:
        occupied.append((WRAPPER_DEST,
                         WRAPPER_DEST + cfg.wrapper_end - cfg.wrapper_start))
    occupied.extend((destination, destination + end - source)
                    for source, end, destination in cfg.extra_maps)
    result = []

    def find_space(size: int, preferred: int | None) -> int:
        candidates = ([preferred] if preferred is not None else []) + \
                     list(range(PAGE_ADDRESS, PAYLOAD_LIMIT, 2))
        for candidate in candidates:
            if candidate is None or candidate + size > PAYLOAD_LIMIT:
                continue
            if all(candidate + size <= a or candidate >= b for a, b in occupied):
                return candidate
        raise ValueError(f"track overlay of {size:X} bytes does not fit page 1")

    allocation_ranges = (sorted(ranges, key=lambda item: item[1] - item[0],
                                reverse=True)
                         if cfg.pack_overlays else ranges)
    for start, end in allocation_ranges:
        size = end - start
        preferred = (start if not cfg.pack_overlays and
                     PAGE_ADDRESS <= start and end <= PAYLOAD_LIMIT
                     else main_destination_end if start == cfg.engine_end
                     else None)
        destination = find_space(size, preferred)
        result.append((start, end, destination))
        occupied.append((destination, destination + size))
    return result


def make_page(image: bytes, cfg: Config, track: int,
              trace_ranges: list[tuple[int, int]], table_entries: int,
              stream_bounds: dict[int, int], main_instructions: list[int],
              wrapper_instructions: list[int],
              indirect_code_targets: set[int] | None = None) -> bytearray:
    page = bytearray(PAGE_SIZE)
    amap = AddressMap()
    main_start = cfg.main_start if cfg.main_start is not None else cfg.load
    main_size = cfg.engine_end - main_start
    main_destination = (cfg.main_destination if cfg.main_destination is not None
                        else (main_start if PAGE_ADDRESS <= main_start and
                              cfg.engine_end <= PAGE_ADDRESS + PAGE_SIZE
                              else PAGE_ADDRESS))
    copy_range(page, image, cfg, amap, main_start, cfg.engine_end,
               main_destination)
    if cfg.wrapper_end is not None:
        copy_range(page, image, cfg, amap, cfg.wrapper_start, cfg.wrapper_end,
                   WRAPPER_DEST)
    amap.add(cfg.work_start, cfg.work_end, cfg.work_dest)
    for source, end, destination in cfg.extra_maps:
        amap.add(source, end, destination)

    descriptor = descriptor_address(image, cfg, track)
    wanted = list(trace_ranges)
    wanted.append((cfg.song_table, cfg.song_table + 2 * table_entries))
    wanted.append((descriptor, descriptor + 18))
    descriptor_bytes = image_slice(image, cfg, descriptor, descriptor + 18)
    for offset in range(2, 18, 2):
        pointer = word(descriptor_bytes, offset)
        if cfg.load <= pointer < cfg.load + len(image):
            # Full-track traces supply the bytes actually consumed from every
            # channel stream (including loop/jump destinations).  Retain the
            # pointer word itself as a safety minimum, but do not extend it to
            # the next game's global stream start: that used to pull unrelated
            # songs into this page and forced otherwise unnecessary moves.
            wanted.append((pointer, min(pointer + 2,
                                        cfg.load + len(image))))

    # Engine and wrapper bytes already live in the template. Workspace and
    # hardware accesses must not become song payload.
    excluded = [(main_start, cfg.engine_end),
                (cfg.work_start, cfg.work_end)]
    if cfg.wrapper_end is not None:
        excluded.append((cfg.wrapper_start, cfg.wrapper_end))
    excluded.extend((source, end) for source, end, _ in cfg.extra_maps)
    filtered = []
    for start, end in wanted:
        start = max(start, cfg.load)
        end = min(end, cfg.load + len(image))
        if end <= start:
            continue
        pieces = [(start, end)]
        for cut_start, cut_end in excluded:
            next_pieces = []
            for piece_start, piece_end in pieces:
                if piece_end <= cut_start or piece_start >= cut_end:
                    next_pieces.append((piece_start, piece_end))
                else:
                    if piece_start < cut_start:
                        next_pieces.append((piece_start, cut_start))
                    if piece_end > cut_end:
                        next_pieces.append((cut_end, piece_end))
            pieces = next_pieces
        filtered.extend(pieces)
    ranges = merged_ranges(filtered)
    placements = allocate_overlay_ranges(cfg, ranges, main_destination,
                                         main_destination + main_size)
    for source, end, destination in placements:
        copy_range(page, image, cfg, amap, source, end, destination)

    # Patch code only after every page-specific data destination is known.
    code_destination = amap.get(cfg.code_start)
    if code_destination is None:
        raise ValueError("code start is not mapped")
    patch_code(page, amap, cfg.code_start, code_destination,
               cfg.engine_end - cfg.code_start,
               main_instructions, cfg.psg_gateway,
               cfg.exhaustive_operand_patch)
    if cfg.wrapper_end is not None:
        patch_code(page, amap, cfg.wrapper_start, WRAPPER_DEST,
                   cfg.wrapper_end - cfg.wrapper_start, wrapper_instructions,
                   exhaustive_operand_patch=cfg.exhaustive_operand_patch)

    # Route direct PSG calls without relying on BIOS page 0.
    # Match the libkss/MSX BIOS WRTPSG stub exactly.  In particular, callers
    # may rely on A being preserved across the write.
    page[cfg.psg_gateway - PAGE_ADDRESS:cfg.psg_gateway - PAGE_ADDRESS + 8] = \
        bytes.fromhex("D3 A0 F5 7B D3 A1 F1 C9")

    # Patch the selected table entry and its 18-byte channel descriptor.
    table_dest = amap.get(cfg.song_table + 2 * track)
    descriptor_dest = amap.get(descriptor)
    if table_dest is None or descriptor_dest is None:
        raise ValueError(f"track {track}: table/descriptor was not mapped")
    put_word(page, table_dest - PAGE_ADDRESS, descriptor_dest)
    for offset in range(2, 18, 2):
        source_pointer = word(image, descriptor - cfg.load + offset)
        destination_pointer = amap.get(source_pointer)
        if destination_pointer is not None:
            put_word(page, descriptor_dest - PAGE_ADDRESS + offset,
                     destination_pointer)

    # Konami sequence commands F9/FD and FB/FC carry absolute stream targets.
    for source, end, destination in placements:
        if not cfg.patch_stream_commands:
            continue
        block = page[destination - PAGE_ADDRESS:destination - PAGE_ADDRESS + end - source]
        for offset, opcode in enumerate(block):
            operand = offset + 1 if opcode in (0xF9, 0xFD) else \
                      offset + 2 if opcode in (0xFB, 0xFC) else None
            if operand is None or operand + 2 > len(block):
                continue
            target = word(block, operand)
            mapped = amap.get(target)
            if mapped is not None:
                put_word(block, operand, mapped)
        page[destination - PAGE_ADDRESS:destination - PAGE_ADDRESS + len(block)] = block

    # Some drivers dispatch through absolute code pointers stored in fixed
    # data tables.  Patch only targets proven executable by a full source
    # trace; this avoids treating arbitrary note/parameter words as pointers.
    if indirect_code_targets:
        for source, end, destination in placements:
            start = destination - PAGE_ADDRESS
            size = end - source
            for offset in range(size - 1):
                target = word(page, start + offset)
                if target not in indirect_code_targets:
                    continue
                mapped = amap.get(target)
                if mapped is not None and mapped != target:
                    put_word(page, start + offset, mapped)

    return page


def overlay_from_template(template: bytes, page: bytes) -> bytes:
    result = bytearray()
    cursor = 0
    while cursor < PAGE_SIZE:
        while cursor < PAGE_SIZE and page[cursor] == template[cursor]:
            cursor += 1
        if cursor == PAGE_SIZE:
            break
        start = cursor
        while cursor < PAGE_SIZE and page[cursor] != template[cursor] and cursor - start < 0xFFFF:
            cursor += 1
        data = page[start:cursor]
        result += struct.pack("<HH", start, len(data)) + data
    return bytes(result)


def common_template(pages: list[bytearray]) -> bytes:
    template = bytearray(pages[0])
    for offset in range(PAGE_SIZE):
        value = template[offset]
        if any(page[offset] != value for page in pages[1:]):
            template[offset] = 0
    return bytes(template)


def info_block(cfg: Config, rows: list[tuple[int, str, int, int, bool]]) -> bytes:
    titles = [f"{cfg.title} [#{track}] - {title}" for track, title, *_ in rows]
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


def compress(packer: Path, name: str, raw: bytes) -> bytes:
    work = ROOT / "tmp" / "konami-complete-pages"
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"{name}.raw"
    destination = work / f"{name}.zx0"
    source.write_bytes(raw)
    subprocess.run([str(packer), str(source), str(destination)], check=True)
    return destination.read_bytes()


def build(stem: str, compressed: bool, destination: Path, packer: Path) -> None:
    cfg = CONFIGS[stem]
    source = ROOT / "vigamup" / f"{stem}.kss"
    trackinfo = ROOT / "vigamup" / f"{stem}.trackinfo"
    trace = ROOT / "vigamup" / "extracted" / f"{stem}.track_extract"
    data = source.read_bytes()
    if data[:4] != b"KSCC" or struct.unpack_from("<HHHH", data, 4) != (
            cfg.load, cfg.load_size, cfg.init, cfg.play):
        raise ValueError(f"{source} is not the expected source KSS")
    image = data[0x10:0x10 + cfg.load_size]
    rows = read_trackinfo(trackinfo)
    traces = read_trace_ranges(trace)
    missing = [track for track, *_ in rows if track not in traces]
    if missing:
        raise ValueError(f"full trace missing tracks: {missing}")
    table_entries = max(track for track, *_ in rows) + 1
    stream_starts = set()
    for track, *_ in rows:
        descriptor = descriptor_address(image, cfg, track)
        descriptor_bytes = image_slice(image, cfg, descriptor, descriptor + 18)
        for offset in range(2, 18, 2):
            pointer = word(descriptor_bytes, offset)
            if cfg.load <= pointer < cfg.load + len(image):
                stream_starts.add(pointer)
    ordered_starts = sorted(stream_starts)
    # The final known stream ends at the end of the loaded image. In these
    # source packs that is at most the C000H wrapper boundary.
    stream_bounds = {
        start: (ordered_starts[index + 1]
                if index + 1 < len(ordered_starts)
                else cfg.load + len(image))
        for index, start in enumerate(ordered_starts)
    }
    main_instructions = disassemble_starts(
        image_slice(image, cfg, cfg.code_start, cfg.engine_end), cfg.code_start,
        f"{stem}-main")
    wrapper_instructions = (disassemble_starts(
        image_slice(image, cfg, cfg.wrapper_start, cfg.wrapper_end),
        cfg.wrapper_start,
        f"{stem}-wrapper") if cfg.wrapper_end is not None else [])
    pages = [make_page(image, cfg, track, traces[track], table_entries,
                       stream_bounds, main_instructions, wrapper_instructions)
             for track, *_ in rows]
    template = common_template(pages)
    overlays = [overlay_from_template(template, page) for page in pages]

    original_map = bytes([row[0] for row in rows] + [0xFF] * (256 - len(rows)))
    page_map = bytes(list(range(len(rows))) + [0xFF] * (256 - len(rows)))
    main_start = cfg.main_start if cfg.main_start is not None else cfg.load
    main_destination = (cfg.main_destination if cfg.main_destination is not None
                        else (main_start if PAGE_ADDRESS <= main_start and
                              cfg.engine_end <= PAGE_ADDRESS + PAGE_SIZE
                              else PAGE_ADDRESS))
    fixed = bytearray(b"KCPZ" if compressed else b"KCPX")
    fixed += bytes((1, len(pages), len(rows), cfg.variant))
    fixed += struct.pack("<HHHH", PAGE_SIZE, PAGE_ADDRESS,
                         cfg.work_dest, 0)
    fixed += original_map + page_map
    assert len(fixed) == FIXED_SIZE

    if compressed:
        packed_template = compress(packer, f"{stem}-template", template)
        packed_overlays = [compress(packer, f"{stem}-overlay-{i}", overlay)
                           for i, overlay in enumerate(overlays)]
        tail = fixed + bytes(4 + len(pages) * 6)
        template_offset = len(tail)
        tail += packed_template
        put_word(tail, FIXED_SIZE, len(packed_template))
        put_word(tail, FIXED_SIZE + 2, template_offset)
        for index, (raw, packed) in enumerate(zip(overlays, packed_overlays)):
            descriptor = FIXED_SIZE + 4 + index * 6
            put_word(tail, descriptor, len(raw))
            put_word(tail, descriptor + 2, len(packed))
            put_word(tail, descriptor + 4, len(tail))
            tail += packed
    else:
        tail = fixed + template
        for overlay in overlays:
            tail += struct.pack("<H", len(overlay)) + overlay

    # Complete-page-aware players select the logical page and translate the
    # public contiguous song index before entering the relocated engine.
    init_address = ((main_destination + cfg.init - main_start)
                    if not (cfg.wrapper_end is not None and
                            cfg.wrapper_start <= cfg.init < cfg.wrapper_end)
                    else WRAPPER_DEST + cfg.init - cfg.wrapper_start)
    play_address = ((main_destination + cfg.play - main_start)
                    if not (cfg.wrapper_end is not None and
                            cfg.wrapper_start <= cfg.play < cfg.wrapper_end)
                    else WRAPPER_DEST + cfg.play - cfg.wrapper_start)
    load_image = b"\xC9"

    header = bytearray(HEADER_SIZE)
    header[:4] = b"KSSX"
    struct.pack_into("<HHHH", header, 4, 0x0200, len(load_image),
                     init_address, play_address)
    header[0x0C] = 0
    header[0x0D] = len(pages)
    header[0x0E] = 0x10
    put_word(header, 0x10, len(load_image) + len(tail))
    put_word(header, 0x18, 0)
    put_word(header, 0x1A, len(rows) - 1)
    result = bytes(header) + load_image + bytes(tail) + info_block(cfg, rows)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    destination.with_suffix(".trackinfo").write_text(
        "\n".join(f"{i},{title},{seconds},{fade},{'yes' if loop else 'no'}"
                  for i, (_, title, seconds, fade, loop) in enumerate(rows)) + "\n",
        encoding="utf-8")
    print(f"wrote {destination} ({len(result)} bytes, {len(rows)} tracks)")
    print(f"  template={len(template)}, overlays={sum(map(len, overlays))}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stem", choices=sorted(CONFIGS))
    parser.add_argument("--uncompressed", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--zx0pack", type=Path, default=ROOT / "build" / "zx0pack")
    args = parser.parse_args()
    suffix = "complete_page.kss" if args.uncompressed else "complete_page_compressed.kss"
    destination = args.output or ROOT / "vigamup" / "extracted" / f"{args.stem}_{suffix}"
    try:
        build(args.stem, not args.uncompressed, destination, args.zx0pack)
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
