#!/usr/bin/env python3
"""Build a Haunted Castle MSX-DOS2 complete-page KSP (KCPX).

The source arcade driver is adapted by build_haunted_castle_full_ksp_v2.py.
This materializer relocates the callable engine, per-song data, wrapper and
working RAM into one 16 KiB page-1 image. Page 2 remains available for a real
SCC cartridge; MoonSound FM is accessed through C4-C7 I/O ports.

The output uses the repository's existing generic KCPX container format.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

AUDIO_SHA1 = "c55f468c0da6afdaa2af65a111583c0c42868bd1"
PAGE_BASE = 0x4000
PAGE_SIZE = 0x4000
FIXED_SIZE = 528
CORE_END = 0x17B0
POINTER_TABLE_START = 0x17B0
POINTER_TABLE_END = 0x1954
TAIL_START = 0x6F04
TAIL_END = 0x8000
WORK_START = 0x8000
WORK_END = 0x8318
OLD_WRAPPER_BASE = 0x8800

# Fixed page-1 layout. The dynamic song resources occupy DATA_START..TAIL_DEST.
CORE_DEST = 0x4000
POINTER_TABLE_DEST = 0x57B0
WRAPPER_DEST = 0x5960
WORK_DEST = 0x5AA0
DATA_START = 0x5DC0
TAIL_DEST = 0x6F00
SONG_WINDOW_DEST = 0x5954

# Two shared resource regions referenced by several song blocks.
COMMON_RESOURCE_RANGES = ((0x437F, 0x47F7), (0x69F5, 0x6B30))

ABSOLUTE_WORD_OPCODES = {
    0x01, 0x11, 0x21, 0x22, 0x2A, 0x31, 0x32, 0x3A,
    0xC2, 0xC3, 0xC4, 0xCA, 0xCC, 0xCD,
    0xD2, 0xD4, 0xDA, 0xDC,
    0xE2, 0xE4, 0xEA, 0xEC,
    0xF2, 0xF4, 0xFA, 0xFC,
}
ED_WORD_OPCODES = {0x43, 0x4B, 0x53, 0x5B, 0x63, 0x6B, 0x73, 0x7B}

# Explicit code-pointer tables used by indirect JP (HL) dispatch.
INDIRECT_CODE_TABLES = (
    (0x07F5, 0x0805),
    (0x081C, 0x0824),
    (0x0B2D, 0x0B3D),
    (0x0B54, 0x0B5C),
    (0x0E45, 0x0E55),
    (0x0E6C, 0x0E74),
)

# Sequence interpreter type by pointer-matrix column.
CHANNEL_GROUP = {0: 1, 1: 1, 2: 1, 3: 2, 4: 3, 5: 0, 6: 1, 7: 1, 8: 1, 9: 2}
MAIN_ARGS = {
    1: {0x08: None, 0x09: 1, 0x0A: 2, 0x0B: 1, 0x0C: 1, 0x0D: 3, 0x0E: 2, 0x0F: 0},
    2: {0x08: None, 0x09: 1, 0x0A: 1, 0x0B: 1, 0x0C: 4, 0x0D: 3, 0x0E: 2, 0x0F: 0},
    3: {0x08: None, 0x09: 1, 0x0A: 1, 0x0B: 0, 0x0C: 0, 0x0D: 3, 0x0E: 1, 0x0F: 0},
}
SUB_ARGS = {
    1: {0: 0, 1: 0, 2: 2, 3: 1},
    2: {0: 0, 1: 0, 2: 1, 3: 0},
    3: {0: 0, 1: 0, 2: 1, 3: 1},
}


@dataclass
class AddressRange:
    source: int
    end: int
    destination: int
    label: str

    @property
    def size(self) -> int:
        return self.end - self.source


class AddressMap:
    def __init__(self) -> None:
        self.ranges: list[AddressRange] = []

    def add(self, source: int, end: int, destination: int, label: str) -> None:
        if not (0 <= source <= end <= 0x10000):
            raise ValueError(f"bad source range {source:04X}-{end:04X}")
        for item in self.ranges:
            if source < item.end and end > item.source:
                raise ValueError(
                    f"overlapping source maps {source:04X}-{end:04X} and "
                    f"{item.source:04X}-{item.end:04X}"
                )
        self.ranges.append(AddressRange(source, end, destination, label))

    def get(self, address: int) -> int | None:
        for item in self.ranges:
            if item.source <= address < item.end:
                return item.destination + address - item.source
        return None


def word(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value & 0xFFFF)


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def import_full_builder(path: Path):
    spec = importlib.util.spec_from_file_location("haunted_castle_full_builder", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse_disassembly(path: Path) -> tuple[list[int], dict[int, int]]:
    starts: list[int] = []
    lengths: dict[int, int] = {}
    pattern = re.compile(r"^([0-9A-Fa-f]{4}):\s+((?:[0-9A-Fa-f]{2}\s+)+)")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        size = len(match.group(2).split())
        if address < CORE_END:
            starts.append(address)
            lengths[address] = size
    return sorted(set(starts)), lengths


def instruction_operand_offset(code: bytes | bytearray, offset: int) -> int | None:
    if offset >= len(code):
        return None
    opcode = code[offset]
    if opcode in ABSOLUTE_WORD_OPCODES:
        return offset + 1
    if opcode in (0xDD, 0xFD) and offset + 1 < len(code) and code[offset + 1] in ABSOLUTE_WORD_OPCODES:
        return offset + 2
    if opcode == 0xED and offset + 1 < len(code) and code[offset + 1] in ED_WORD_OPCODES:
        return offset + 2
    return None


def merged_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    result: list[list[int]] = []
    for start, end in sorted(ranges):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1][1] = max(result[-1][1], end)
        else:
            result.append([start, end])
    return [(a, b) for a, b in result]


def subtract_ranges(ranges: Iterable[tuple[int, int]], cuts: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    pieces = list(ranges)
    for cut_start, cut_end in cuts:
        updated: list[tuple[int, int]] = []
        for start, end in pieces:
            if end <= cut_start or start >= cut_end:
                updated.append((start, end))
            else:
                if start < cut_start:
                    updated.append((start, cut_start))
                if end > cut_end:
                    updated.append((cut_end, end))
        pieces = updated
    return merged_ranges(pieces)


def parse_stream(image: bytes, start: int, group: int) -> tuple[int, list[tuple[int, int]], list[tuple[int, int]]]:
    """Return end, embedded jump pointers, and instrument pointers.

    Pointer tuples contain (operand address, target). The 0DH jump terminates
    the current linear stream; its target is another stream for the same
    interpreter group.
    """
    cursor = start
    jumps: list[tuple[int, int]] = []
    resources: list[tuple[int, int]] = []
    for _ in range(0x10000):
        if cursor >= len(image):
            raise ValueError(f"stream {start:04X} runs outside audio ROM")
        command = image[cursor]
        cursor += 1
        if command & 0xF0 or command < 8:
            continue
        if command not in MAIN_ARGS[group]:
            raise ValueError(f"unknown group-{group} command {command:02X} at {cursor - 1:04X}")
        if command == 0x0F:
            return cursor, jumps, resources
        if command == 0x0D:
            if cursor + 3 > len(image):
                raise ValueError("truncated 0DH sequence command")
            operand = cursor + 1
            target = word(image, operand)
            jumps.append((operand, target))
            cursor += 3
            return cursor, jumps, resources
        if command == 0x0E:
            if group in (1, 2):
                resources.append((cursor, word(image, cursor)))
                cursor += 2
            else:
                cursor += 1
            continue
        if command == 0x08:
            subcommand = image[cursor]
            cursor += 1
            if subcommand not in SUB_ARGS[group]:
                raise ValueError(
                    f"unknown group-{group} command 08/{subcommand:02X} at {cursor - 2:04X}"
                )
            cursor += SUB_ARGS[group][subcommand]
            continue
        cursor += MAIN_ARGS[group][command]
    raise ValueError(f"stream {start:04X} did not terminate")


def collect_stream_metadata(image: bytes, roots: list[int]) -> tuple[list[tuple[int, int]], dict[int, int], dict[int, int]]:
    ranges: list[tuple[int, int]] = []
    jump_operands: dict[int, int] = {}
    resource_operands: dict[int, int] = {}
    pending: list[tuple[int, int]] = []
    for column, root in enumerate(roots):
        group = CHANNEL_GROUP[column]
        if group:
            pending.append((root, group))
    visited: set[tuple[int, int]] = set()
    while pending:
        start, group = pending.pop()
        key = (start, group)
        if key in visited:
            continue
        visited.add(key)
        end, jumps, resources = parse_stream(image, start, group)
        ranges.append((start, end))
        for operand, target in jumps:
            jump_operands[operand] = target
            pending.append((target, group))
        for operand, target in resources:
            resource_operands[operand] = target
    return merged_ranges(ranges), jump_operands, resource_operands


def copy_mapped(page: bytearray, source: bytes, amap: AddressMap, start: int, end: int, destination: int, label: str) -> None:
    if not (PAGE_BASE <= destination and destination + end - start <= PAGE_BASE + PAGE_SIZE):
        raise ValueError(f"{label} does not fit page 1")
    page[destination - PAGE_BASE:destination - PAGE_BASE + end - start] = source[start:end]
    amap.add(start, end, destination, label)


def patch_core(page: bytearray, source_core: bytes, amap: AddressMap, starts: list[int]) -> dict[str, int]:
    core_offset = CORE_DEST - PAGE_BASE
    patched_operands = 0
    for address in starts:
        if address >= CORE_END:
            continue
        local = address
        operand = instruction_operand_offset(source_core, local)
        if operand is None or operand + 2 > len(source_core):
            continue
        value = word(source_core, operand)
        # Physical SCC and arcade hardware addresses are intentionally not mapped.
        replacement = amap.get(value)
        if replacement is None or replacement == value:
            continue
        put_word(page, core_offset + operand, replacement)
        patched_operands += 1

    patched_table_words = 0
    for start, end in INDIRECT_CODE_TABLES:
        for address in range(start, end, 2):
            value = word(source_core, address)
            replacement = amap.get(value)
            if replacement is None:
                raise ValueError(f"unmapped code pointer {value:04X} at {address:04X}")
            put_word(page, core_offset + address, replacement)
            patched_table_words += 1
    return {"instruction_operands": patched_operands, "indirect_code_words": patched_table_words}


def replace_word_all(data: bytearray, old: int, new: int, expected: int | None = None) -> int:
    old_bytes = struct.pack("<H", old)
    new_bytes = struct.pack("<H", new)
    count = 0
    cursor = 0
    while True:
        found = data.find(old_bytes, cursor)
        if found < 0:
            break
        data[found:found + 2] = new_bytes
        count += 1
        cursor = found + 2
    if expected is not None and count != expected:
        raise ValueError(f"wrapper address {old:04X}: patched {count}, expected {expected}")
    return count


def build_relocated_wrapper(full_builder, amap: AddressMap) -> tuple[bytes, dict[str, int], dict[str, int]]:
    old_base = full_builder.WRAPPER_BASE
    old_song_window = full_builder.SONG_WINDOW
    try:
        full_builder.WRAPPER_BASE = WRAPPER_DEST
        full_builder.SONG_WINDOW = SONG_WINDOW_DEST
        wrapper, labels = full_builder.build_wrapper()
    finally:
        full_builder.WRAPPER_BASE = old_base
        full_builder.SONG_WINDOW = old_song_window

    relocated = bytearray(wrapper)
    counts: dict[str, int] = {}
    for old, expected in ((0x02FD, 1), (0x003C, 1), (0x0476, 1), (0x0F06, 1),
                          (0x8315, 3), (0x8316, 3), (0x8317, 3)):
        new = amap.get(old)
        if new is None:
            raise ValueError(f"wrapper dependency {old:04X} is not mapped")
        counts[f"{old:04X}->{new:04X}"] = replace_word_all(relocated, old, new, expected)
    return bytes(relocated), labels, counts


def common_template(pages: list[bytearray]) -> bytes:
    template = bytearray(pages[0])
    for offset in range(PAGE_SIZE):
        value = template[offset]
        if any(page[offset] != value for page in pages[1:]):
            template[offset] = 0
    return bytes(template)


def overlay_from_template(template: bytes, page: bytes) -> bytes:
    result = bytearray()
    cursor = 0
    while cursor < PAGE_SIZE:
        while cursor < PAGE_SIZE and page[cursor] == template[cursor]:
            cursor += 1
        if cursor == PAGE_SIZE:
            break
        start = cursor
        while cursor < PAGE_SIZE and page[cursor] != template[cursor]:
            cursor += 1
        payload = page[start:cursor]
        result += struct.pack("<HH", start, len(payload)) + payload
    return bytes(result)


def apply_overlay(template: bytes, overlay: bytes) -> bytes:
    page = bytearray(template)
    cursor = 0
    while cursor < len(overlay):
        if cursor + 4 > len(overlay):
            raise ValueError("truncated overlay header")
        offset, size = struct.unpack_from("<HH", overlay, cursor)
        cursor += 4
        if cursor + size > len(overlay) or offset + size > PAGE_SIZE:
            raise ValueError("invalid overlay record")
        page[offset:offset + size] = overlay[cursor:cursor + size]
        cursor += size
    return bytes(page)


def info_block(song_count: int) -> bytes:
    titles = [f"Haunted Castle [command {0x50 + song:02X}H]" for song in range(song_count)]
    result = bytearray(0x10 + sum(10 + len(title.encode("utf-8")) + 1 for title in titles))
    result[:4] = b"INFO"
    put_word(result, 8, song_count)
    cursor = 0x10
    for song, title in enumerate(titles):
        result[cursor] = song
        result[cursor + 1] = 1  # looping/unknown: let player continue
        struct.pack_into("<II", result, cursor + 2, 180000, 5000)
        cursor += 10
        encoded = title.encode("utf-8") + b"\0"
        result[cursor:cursor + len(encoded)] = encoded
        cursor += len(encoded)
    put_word(result, 4, len(result) - 0x10)
    return bytes(result)


def build_page(song: int, engine: bytes, original_audio: bytes, full_builder, starts: list[int]) -> tuple[bytearray, dict]:
    roots = [word(original_audio, POINTER_TABLE_START + (song * 10 + i) * 2) for i in range(10)]
    all_roots = sorted({word(original_audio, POINTER_TABLE_START + i * 2) for i in range(210)})
    next_root = {root: (all_roots[i + 1] if i + 1 < len(all_roots) else TAIL_END)
                 for i, root in enumerate(all_roots)}
    primary_start = min(roots)
    primary_end = next_root[max(roots)]

    stream_ranges, jump_operands, resource_operands = collect_stream_metadata(original_audio, roots)

    wanted: list[tuple[int, int]] = [(primary_start, primary_end)]
    wanted.extend(stream_ranges)

    # Add common resource groups only when a selected stream actually references them.
    resource_targets = set(resource_operands.values())
    for common_start, common_end in COMMON_RESOURCE_RANGES:
        if any(common_start <= target < common_end for target in resource_targets):
            wanted.append((common_start, common_end))

    fixed_source_ranges = (
        (0x0000, CORE_END),
        (POINTER_TABLE_START, POINTER_TABLE_END),
        (TAIL_START, TAIL_END),
        (WORK_START, WORK_END),
        (OLD_WRAPPER_BASE, OLD_WRAPPER_BASE + (len(engine) - OLD_WRAPPER_BASE)),
    )
    dynamic_ranges = subtract_ranges(merged_ranges(wanted), fixed_source_ranges)

    page = bytearray(PAGE_SIZE)
    amap = AddressMap()
    copy_mapped(page, engine, amap, 0x0000, CORE_END, CORE_DEST, "engine")
    copy_mapped(page, engine, amap, POINTER_TABLE_START, POINTER_TABLE_END,
                POINTER_TABLE_DEST, "pointer matrix")
    amap.add(WORK_START, WORK_END, WORK_DEST, "work RAM")
    # Work RAM is already zero in the new page.
    copy_mapped(page, engine, amap, TAIL_START, TAIL_END, TAIL_DEST, "shared tail")

    cursor = DATA_START
    placements: list[dict] = []
    for start, end in dynamic_ranges:
        if cursor + (end - start) > TAIL_DEST:
            raise ValueError(
                f"song {song}: dynamic data exceeds page 1: need {end - start} bytes "
                f"at {cursor:04X}, tail starts {TAIL_DEST:04X}"
            )
        copy_mapped(page, engine, amap, start, end, cursor, f"song resource {start:04X}")
        placements.append({"source": f"0x{start:04X}", "end": f"0x{end:04X}",
                           "destination": f"0x{cursor:04X}", "size": end - start})
        cursor += end - start

    wrapper, labels, wrapper_counts = build_relocated_wrapper(full_builder, amap)
    if WRAPPER_DEST + len(wrapper) > WORK_DEST:
        raise ValueError("relocated wrapper overlaps work RAM")
    page[WRAPPER_DEST - PAGE_BASE:WRAPPER_DEST - PAGE_BASE + len(wrapper)] = wrapper
    amap.add(OLD_WRAPPER_BASE, OLD_WRAPPER_BASE + len(wrapper), WRAPPER_DEST, "wrapper")

    patch_counts = patch_core(page, engine[:CORE_END], amap, starts)

    # Patch the selected row in the relocated pointer matrix.
    for column, root in enumerate(roots):
        mapped = amap.get(root)
        if mapped is None:
            raise ValueError(f"song {song}: root {root:04X} is not materialized")
        put_word(page, POINTER_TABLE_DEST - PAGE_BASE + (song * 10 + column) * 2, mapped)

    # Patch sequence jump and resource-pointer operands reached by this song.
    stream_patch_count = 0
    for source_operand, target in {**jump_operands, **resource_operands}.items():
        operand_dest = amap.get(source_operand)
        target_dest = amap.get(target)
        if operand_dest is None:
            # Operand may live in a stream that was discovered but was already
            # covered by the fixed core; this should still be mapped.
            raise ValueError(f"song {song}: pointer operand {source_operand:04X} is not mapped")
        if target_dest is None:
            raise ValueError(
                f"song {song}: sequence target {target:04X} from {source_operand:04X} is not mapped"
            )
        put_word(page, operand_dest - PAGE_BASE, target_dest)
        stream_patch_count += 1

    page[SONG_WINDOW_DEST - PAGE_BASE] = song

    # Static validation: executable operands may no longer point at old mapped locations.
    stale: list[dict] = []
    for address in starts:
        if address >= CORE_END:
            continue
        operand = instruction_operand_offset(engine[:CORE_END], address)
        if operand is None or operand + 2 > CORE_END:
            continue
        old_value = word(engine, operand)
        if amap.get(old_value) is None:
            continue
        new_value = word(page, CORE_DEST - PAGE_BASE + operand)
        if new_value == old_value:
            stale.append({"instruction": f"0x{address:04X}", "value": f"0x{old_value:04X}"})
    if stale:
        raise ValueError(f"song {song}: stale relocated operands: {stale[:8]}")

    return page, {
        "song": song,
        "command": f"0x{0x50 + song:02X}",
        "roots": [f"0x{x:04X}" for x in roots],
        "primary_range": [f"0x{primary_start:04X}", f"0x{primary_end:04X}"],
        "dynamic_end": f"0x{cursor:04X}",
        "dynamic_capacity_end": f"0x{TAIL_DEST:04X}",
        "dynamic_free": TAIL_DEST - cursor,
        "placements": placements,
        "jump_pointers": len(jump_operands),
        "resource_pointers": len(resource_operands),
        "stream_pointers_patched": stream_patch_count,
        "core_patches": patch_counts,
        "wrapper_address_patches": wrapper_counts,
        "init": f"0x{labels['init']:04X}",
        "play": f"0x{labels['play']:04X}",
        "stop": f"0x{labels['stop']:04X}",
        "page_sha1": sha1(bytes(page)),
    }


def write_kcpx(destination: Path, pages: list[bytearray], init_address: int, play_address: int) -> tuple[bytes, bytes, list[bytes]]:
    template = common_template(pages)
    overlays = [overlay_from_template(template, page) for page in pages]
    for index, (page, overlay) in enumerate(zip(pages, overlays)):
        if apply_overlay(template, overlay) != bytes(page):
            raise ValueError(f"overlay reconstruction failed for song {index}")

    original_map = bytes(list(range(len(pages))) + [0xFF] * (256 - len(pages)))
    page_map = bytes(list(range(len(pages))) + [0xFF] * (256 - len(pages)))
    fixed = bytearray(b"KCPX")
    fixed += bytes((1, len(pages), len(pages), 10))  # variant 10: Haunted Castle
    fixed += struct.pack("<HHHH", PAGE_SIZE, PAGE_BASE, WORK_DEST, 0)
    fixed += original_map + page_map
    if len(fixed) != FIXED_SIZE:
        raise AssertionError("bad KCPX fixed header size")

    tail = fixed + template
    for overlay in overlays:
        tail += struct.pack("<H", len(overlay)) + overlay

    load_image = b"\xC9"
    header = bytearray(0x20)
    header[:4] = b"KSSX"
    struct.pack_into("<HHHH", header, 4, 0x0200, len(load_image), init_address, play_address)
    header[0x0C] = 0
    header[0x0D] = len(pages)
    header[0x0E] = 0x10
    put_word(header, 0x10, len(load_image) + len(tail))
    put_word(header, 0x18, 0)
    put_word(header, 0x1A, len(pages) - 1)

    result = bytes(header) + load_image + bytes(tail) + info_block(len(pages))
    destination.write_bytes(result)
    return result, template, overlays


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audio-rom", type=Path, default=Path("768e01.e4"))
    parser.add_argument("--full-builder", type=Path, default=Path("build_haunted_castle_full_ksp_v2.py"))
    parser.add_argument("--disassembly", type=Path,
                        default=Path("haunted_castle_recursive_disassembly.txt"))
    parser.add_argument("--output", type=Path, default=Path("haunted_castle_msx_complete_page.ksp"))
    parser.add_argument("--pages-dir", type=Path, default=Path("haunted_castle_msx_pages"))
    parser.add_argument("--report", type=Path, default=Path("haunted_castle_msx_complete_page_validation.json"))
    args = parser.parse_args()

    audio = args.audio_rom.read_bytes()
    if len(audio) != 0x8000 or sha1(audio) != AUDIO_SHA1:
        raise SystemExit("unsupported or corrupt 768e01.e4")

    full_builder = import_full_builder(args.full_builder)
    engine, _ = full_builder.build_engine(audio)
    starts, _ = parse_disassembly(args.disassembly)

    pages: list[bytearray] = []
    reports: list[dict] = []
    args.pages_dir.mkdir(parents=True, exist_ok=True)
    for song in range(21):
        page, report = build_page(song, engine, audio, full_builder, starts)
        pages.append(page)
        reports.append(report)
        (args.pages_dir / f"haunted_castle_song_{song:02d}_page1.bin").write_bytes(page)

    # Wrapper address is fixed in every page.
    old_base = full_builder.WRAPPER_BASE
    old_song = full_builder.SONG_WINDOW
    try:
        full_builder.WRAPPER_BASE = WRAPPER_DEST
        full_builder.SONG_WINDOW = SONG_WINDOW_DEST
        _, labels = full_builder.build_wrapper()
    finally:
        full_builder.WRAPPER_BASE = old_base
        full_builder.SONG_WINDOW = old_song

    result, template, overlays = write_kcpx(args.output, pages, labels["init"], labels["play"])
    (args.pages_dir / "haunted_castle_common_template.bin").write_bytes(template)
    for song, overlay in enumerate(overlays):
        (args.pages_dir / f"haunted_castle_song_{song:02d}.overlay").write_bytes(overlay)

    report = {
        "format": "KCPX v1",
        "output": str(args.output),
        "output_size": len(result),
        "output_sha1": sha1(result),
        "audio_sha1": sha1(audio),
        "page_layout": {
            "engine": [f"0x{CORE_DEST:04X}", f"0x{CORE_DEST + CORE_END:04X}"],
            "pointer_matrix": [f"0x{POINTER_TABLE_DEST:04X}",
                               f"0x{POINTER_TABLE_DEST + POINTER_TABLE_END - POINTER_TABLE_START:04X}"],
            "wrapper": [f"0x{WRAPPER_DEST:04X}", f"0x{WRAPPER_DEST + len(engine) - OLD_WRAPPER_BASE:04X}"],
            "work_ram": [f"0x{WORK_DEST:04X}", f"0x{WORK_DEST + WORK_END - WORK_START:04X}"],
            "dynamic_data": [f"0x{DATA_START:04X}", f"0x{TAIL_DEST:04X}"],
            "shared_tail": [f"0x{TAIL_DEST:04X}", f"0x{TAIL_DEST + TAIL_END - TAIL_START:04X}"],
            "page2": "real SCC cartridge; not occupied by page image",
            "moonsound": "I/O C4-C7; no CPU memory page",
        },
        "init_address": f"0x{labels['init']:04X}",
        "play_address": f"0x{labels['play']:04X}",
        "page_count": len(pages),
        "template_size": len(template),
        "overlay_total": sum(len(x) for x in overlays),
        "largest_overlay": max(len(x) for x in overlays),
        "minimum_dynamic_free": min(item["dynamic_free"] for item in reports),
        "songs": reports,
        "limitations": [
            "K007232 accesses still use the virtual D0/D1 gateway; the current MSX player needs an OPL4 PCM translator for those ports.",
            "This build is statically relocation-validated and KCPX reconstruction-validated; audio output still requires an OpenMSX or real-MSX playback test.",
        ],
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.output} ({len(result)} bytes, SHA-1 {sha1(result)})")
    print(f"all {len(pages)} songs fit; minimum free dynamic page space: {report['minimum_dynamic_free']} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
