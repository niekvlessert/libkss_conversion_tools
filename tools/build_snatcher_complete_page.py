#!/usr/bin/env python3
"""Build one-page KCPX/KCPZ images for the original Snatcher SCC pack.

The Snatcher driver is kept at one uniform relocation in page 1.  Music is
packed into trace-proven unused holes in that image and into the remaining
tail of the page.  Large tracks factor identical complete command passages
through the driver's native F9 (phrase call) and FA (phrase return) commands.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import struct

from build_konami_complete_pages import (
    AddressMap, Config, PAGE_ADDRESS, PAGE_SIZE, WRAPPER_DEST,
    disassemble_starts, merged_ranges, patch_code, put_word, read_trackinfo,
)
from build_konami_banked_complete_pages import write_container


ROOT = Path(__file__).resolve().parents[1]
MAIN_START = 0x018C
MAIN_END = 0x2108
MAIN_DEST = 0x4000
MAIN_DELTA = MAIN_DEST - MAIN_START
WRAPPER_START = 0xC268
WRAPPER_END = 0xC2FE
PSG_GATEWAY = 0x6020
TAIL_START = PSG_GATEWAY + 8
PAGE_END = 0x8000

CFG = Config(
    10, "Snatcher", 0x00D0, 0xC22E, 0xC268, 0xC2FA,
    0x01CB, MAIN_START, MAIN_END, WRAPPER_END,
    0x1DD8, 0x2042, 0,
    True, MAIN_START, PSG_GATEWAY,
    main_destination=MAIN_DEST, wrapper_start=WRAPPER_START,
    traced_stream_commands=True,
)

CONTROL_LENGTHS = {0xF9: 3, 0xFB: 4, 0xFC: 4, 0xFD: 3}
CONTROL_OPERANDS = {0xF9: 1, 0xFB: 2, 0xFC: 2, 0xFD: 1}
COMMAND_DISPATCH_START = 0x0E37
# FF is handled before dispatch, so the table ends at index 46 (FE).
COMMAND_DISPATCH_ENTRIES = 47
NOTE_POINTER_TABLES = ((0x1B5A, 10), (0x1C37, 12))
WAVE_POINTER_TABLES = ((0x16FE, 48), (0x179E, 22))


@dataclass(frozen=True)
class TrackTrace:
    engine_ranges: tuple[tuple[int, int], ...]
    executed_ranges: tuple[tuple[int, int], ...]
    bank_ranges: dict[int, tuple[tuple[int, int], ...]]
    boundaries: frozenset[int]


@dataclass(frozen=True)
class Phrase:
    first: int
    second: int
    length: int


@dataclass
class Node:
    data: bytearray
    sources: list[int | None]
    boundaries: set[int]
    shared_source_start: int | None = None


@dataclass
class Piece:
    node: Node
    node_start: int
    node_end: int
    destination: int
    jump_destination: int | None = None


def parse_ranges(value: str) -> list[tuple[int, int]]:
    if value == "none":
        return []
    result = []
    for item in value.split(","):
        start, end = item.split("-")
        result.append((int(start, 16), int(end, 16)))
    return result


def read_traces(path: Path) -> dict[int, TrackTrace]:
    result: dict[int, TrackTrace] = {}
    current: int | None = None
    engine: list[tuple[int, int]] = []
    executed: list[tuple[int, int]] = []
    banks: dict[int, tuple[tuple[int, int], ...]] = {}
    boundaries: set[int] = set()

    def finish() -> None:
        if current is not None:
            result[current] = TrackTrace(
                tuple(merged_ranges(engine, gap=0)),
                tuple(merged_ranges(executed, gap=0)), dict(banks),
                frozenset(boundaries))

    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("track="):
            finish()
            current = int(line[6:])
            engine = []
            executed = []
            banks = {}
            boundaries = set()
        elif current is not None and line.startswith(
                ("executed_ranges=", "data_read_ranges=",
                 "memory_write_ranges=")):
            ranges = parse_ranges(line.split("=", 1)[1])
            engine.extend(ranges)
            if line.startswith("executed_ranges="):
                executed.extend(ranges)
        elif current is not None and line.startswith("bank_data_read_ranges_"):
            label, value = line.split("=", 1)
            bank = int(label.rsplit("_", 1)[1])
            banks[bank] = tuple(parse_ranges(value))
        elif current is not None and line.startswith("stream_command_ranges="):
            for start, end in parse_ranges(line.split("=", 1)[1]):
                boundaries.update(range(start, end))
    finish()
    return result


def image_slice(engine: bytes, start: int, end: int) -> bytes:
    return engine[start - CFG.load:end - CFG.load]


def active_bank(trace: TrackTrace) -> int:
    active = [bank for bank, ranges in trace.bank_ranges.items() if ranges]
    if len(active) != 1:
        raise ValueError(f"expected one active Snatcher bank, got {active}")
    return active[0]


def source_byte(engine: bytes, bank: bytes, address: int) -> int:
    return (bank[address - 0x8000] if address >= 0x8000 else
            engine[address - CFG.load])


def source_word(engine: bytes, bank: bytes, address: int) -> int:
    return source_byte(engine, bank, address) | \
        (source_byte(engine, bank, address + 1) << 8)


def stream_target(engine: bytes, bank: bytes, command: int) -> int | None:
    opcode = source_byte(engine, bank, command)
    length = CONTROL_LENGTHS.get(opcode)
    if length is None:
        return None
    operand = command + CONTROL_OPERANDS[opcode]
    distance = source_word(engine, bank, operand)
    return (command + length - distance) & 0xFFFF


def descriptor_targets(engine: bytes, bank: bytes, track: int) -> tuple[int, set[int]]:
    table = CFG.song_table + 2 * track
    descriptor = struct.unpack_from("<H", engine, table - CFG.load)[0]
    source = (bank if 0x8000 <= descriptor < 0xC000 else engine)
    base = descriptor - (0x8000 if source is bank else CFG.load)
    if base < 0 or base + 18 > len(source):
        raise ValueError(f"track {track}: descriptor {descriptor:04X} is outside image")
    targets = set()
    for offset in range(2, 18, 2):
        relative = struct.unpack_from("<H", source, base + offset)[0]
        if relative != 0xFFFF:
            targets.add((descriptor + relative) & 0xFFFF)
    return descriptor, targets


def find_phrases(engine: bytes, bank: bytes,
                 ranges: tuple[tuple[int, int], ...],
                 boundaries: frozenset[int], external_targets: set[int]) -> list[Phrase]:
    readable = set()
    for start, end in ranges:
        readable.update(range(0x8000 + start, 0x8000 + end))
    lo = min(readable)
    hi = max(readable) + 1
    seed = 16
    seen: dict[bytes, list[int]] = {}
    pairs: set[tuple[int, int]] = set()
    for address in range(lo, hi - seed + 1):
        if not all(address + i in readable for i in range(seed)):
            continue
        key = bank[address - 0x8000:address - 0x8000 + seed]
        previous = seen.setdefault(key, [])
        for other in previous[-8:]:
            pairs.add((other, address))
        previous.append(address)

    controls = []
    for command in boundaries:
        if not 0x8000 <= command < 0xC000:
            continue
        target = stream_target(engine, bank, command)
        if target is not None:
            controls.append((command, bank[command - 0x8000], target))

    candidates: set[Phrase] = set()
    for first, second in pairs:
        while (first > lo and second > lo and first - 1 in readable and
               second - 1 in readable and first - 1 != second and
               bank[first - 0x8001] == bank[second - 0x8001]):
            first -= 1
            second -= 1
        length = 0
        while (second + length < hi and first + length < second and
               first + length in readable and second + length in readable and
               bank[first - 0x8000 + length] ==
               bank[second - 0x8000 + length]):
            length += 1
        if length < seed or first not in boundaries or second not in boundaries or \
                first + length not in boundaries or second + length not in boundaries:
            continue
        safe = True
        for start in (first, second):
            end = start + length
            if any(start < target < end for target in external_targets):
                safe = False
                break
            for command, opcode, target in controls:
                if start <= command < end:
                    # F9 has only one return slot and therefore cannot nest.
                    if opcode == 0xF9 or not (start <= target < end):
                        safe = False
                        break
                # A generated F9 at an existing phrase entry would nest in
                # the original F9 call and overwrite IX+7/8, Snatcher's sole
                # phrase-return slot. Reject external control-flow entries at
                # the first byte as well as entries into the interior.
                elif start <= target < end:
                    safe = False
                    break
            if any(bank[address - 0x8000] in (0xFA, 0xFF)
                   for address in boundaries if start <= address < end):
                safe = False
            if not safe:
                break
        if safe:
            candidates.add(Phrase(first, second, length))

    used: list[tuple[int, int]] = []
    selected = []
    for phrase in sorted(candidates, key=lambda item: item.length, reverse=True):
        intervals = ((phrase.first, phrase.first + phrase.length),
                     (phrase.second, phrase.second + phrase.length))
        if any(not (end <= a or start >= b)
               for start, end in intervals for a, b in used):
            continue
        selected.append(phrase)
        used.extend(intervals)
    return selected


def required_mask(trace: TrackTrace) -> set[int]:
    protected = set()
    for start, end in trace.engine_ranges:
        start = max(start, MAIN_START)
        end = min(end, MAIN_END)
        if end > start:
            protected.update(range(start, end))
    # The trace bitmap records the Z80 instruction address (t_pc), not every
    # immediate/displacement byte fetched by that instruction. Preserve the
    # complete longest Z80 encoding following each trace-proven address.
    expanded = set(protected)
    for address in protected:
        expanded.update(range(address, min(address + 4, MAIN_END)))
    return expanded


def free_intervals(protected: set[int]) -> list[tuple[int, int]]:
    holes = []
    source = MAIN_START
    while source < MAIN_END:
        while source < MAIN_END and source in protected:
            source += 1
        start = source
        while source < MAIN_END and source not in protected:
            source += 1
        if source - start >= 8:
            holes.append((MAIN_DEST + start - MAIN_START,
                          MAIN_DEST + source - MAIN_START))
    holes.append((TAIL_START, PAGE_END))
    return sorted(holes, key=lambda item: item[1] - item[0], reverse=True)


def make_nodes(bank: bytes, ranges: tuple[tuple[int, int], ...],
               boundaries: frozenset[int], phrases: list[Phrase]) -> tuple[
                       list[Node], dict[int, tuple[Phrase, int]]]:
    occurrence: dict[int, Phrase] = {}
    interior: dict[int, tuple[Phrase, int]] = {}
    for phrase in phrases:
        for start in (phrase.first, phrase.second):
            occurrence[start] = phrase
            for offset in range(phrase.length):
                interior[start + offset] = (phrase, offset)
    nodes = []
    for raw_start, raw_end in ranges:
        start, end = 0x8000 + raw_start, 0x8000 + raw_end
        data = bytearray()
        sources: list[int | None] = []
        node_boundaries = set()
        address = start
        while address < end:
            phrase = occurrence.get(address)
            if phrase is not None and address + phrase.length <= end:
                node_boundaries.add(len(data))
                data += b"\xF9\0\0"
                sources += [address, None, None]
                address += phrase.length
                continue
            if address in boundaries:
                node_boundaries.add(len(data))
            data.append(bank[address - 0x8000])
            sources.append(address)
            address += 1
        node_boundaries.add(len(data))
        nodes.append(Node(data, sources, node_boundaries))
    for phrase in phrases:
        data = bytearray(bank[phrase.first - 0x8000:
                              phrase.first - 0x8000 + phrase.length])
        sources = list(range(phrase.first, phrase.first + phrase.length))
        node_boundaries = {address - phrase.first for address in boundaries
                           if phrase.first <= address <= phrase.first + phrase.length}
        node_boundaries.add(len(data))
        data.append(0xFA)
        sources.append(None)
        nodes.append(Node(data, sources, node_boundaries,
                          shared_source_start=phrase.first))
    return nodes, interior


def make_external_nodes(engine: bytes, trace: TrackTrace) -> list[Node]:
    nodes = []
    for start, end in trace.engine_ranges:
        start = max(start, MAIN_END)
        end = min(end, 0x8000)
        if end <= start:
            continue
        data = bytearray(image_slice(engine, start, end))
        sources = list(range(start, end))
        boundaries = {address - start for address in trace.boundaries
                      if start <= address <= end}
        boundaries.update((0, len(data)))
        nodes.append(Node(data, sources, boundaries))
    return nodes


def allocate_nodes(nodes: list[Node], intervals: list[tuple[int, int]]) -> list[Piece]:
    free = [list(item) for item in intervals]
    pieces: list[Piece] = []
    # Long source ranges first; small shared phrases naturally fill holes.
    for node in sorted(nodes, key=lambda item: len(item.data), reverse=True):
        cursor = 0
        while cursor < len(node.data):
            free.sort(key=lambda item: item[1] - item[0], reverse=True)
            if not free or free[0][1] - free[0][0] < 4:
                raise ValueError("packed Snatcher music does not fit page 1")
            start, end = free[0]
            capacity = end - start
            remaining = len(node.data) - cursor
            if remaining <= capacity:
                piece_end = len(node.data)
                free[0][0] += remaining
                pieces.append(Piece(node, cursor, piece_end, start))
                cursor = piece_end
                continue
            # Leave three bytes for an FD jump to the next physical piece.
            candidates = [boundary for boundary in node.boundaries
                          if cursor < boundary <= cursor + capacity - 3]
            if not candidates:
                free.pop(0)
                continue
            piece_end = max(candidates)
            used = piece_end - cursor + 3
            free[0][0] += used
            pieces.append(Piece(node, cursor, piece_end, start,
                                jump_destination=-1))
            cursor = piece_end
    by_node: dict[int, list[Piece]] = {}
    for piece in pieces:
        by_node.setdefault(id(piece.node), []).append(piece)
    for node_pieces in by_node.values():
        node_pieces.sort(key=lambda piece: piece.node_start)
        for index, piece in enumerate(node_pieces[:-1]):
            if piece.jump_destination is not None:
                piece.jump_destination = node_pieces[index + 1].destination
    return pieces


def build_page(engine: bytes, banks: list[bytes], track: int,
               trace: TrackTrace) -> tuple[bytearray, int, int]:
    bank_index = active_bank(trace)
    bank = banks[bank_index]
    descriptor, descriptor_streams = descriptor_targets(engine, bank, track)
    ranges = trace.bank_ranges[bank_index]
    candidates = find_phrases(engine, bank, ranges, trace.boundaries,
                              descriptor_streams | {descriptor})
    protected = required_mask(trace)
    intervals = free_intervals(protected)

    # Keep ordinary tracks byte-for-byte structurally equivalent.  F9 has a
    # single return slot in the Snatcher driver, so factoring passages that do
    # not need factoring only adds risk without saving a mapper page.  Add the
    # largest independent phrases one at a time and stop at the first layout
    # that fits.
    phrases: list[Phrase] = []
    for count in range(len(candidates) + 1):
        phrases = candidates[:count]
        nodes, interiors = make_nodes(bank, ranges, trace.boundaries, phrases)
        nodes.extend(make_external_nodes(engine, trace))
        try:
            pieces = allocate_nodes(nodes, intervals)
            break
        except ValueError:
            if count == len(candidates):
                raise

    page = bytearray(PAGE_SIZE)
    page[:MAIN_END - MAIN_START] = image_slice(engine, MAIN_START, MAIN_END)
    wrapper_offset = WRAPPER_DEST - PAGE_ADDRESS
    page[wrapper_offset:wrapper_offset + WRAPPER_END - WRAPPER_START] = \
        image_slice(engine, WRAPPER_START, WRAPPER_END)
    page[PSG_GATEWAY - PAGE_ADDRESS:PSG_GATEWAY - PAGE_ADDRESS + 8] = \
        bytes.fromhex("D3 A0 F5 7B D3 A1 F1 C9")

    source_map: dict[int, int] = {}
    shared_map: dict[int, int] = {}
    phrase_destination: dict[Phrase, int] = {}
    for piece in pieces:
        destination = piece.destination
        payload = piece.node.data[piece.node_start:piece.node_end]
        offset = destination - PAGE_ADDRESS
        page[offset:offset + len(payload)] = payload
        for index in range(piece.node_start, piece.node_end):
            source = piece.node.sources[index]
            if source is not None:
                target = destination + index - piece.node_start
                if piece.node.shared_source_start is None:
                    source_map[source] = target
                else:
                    shared_map[source] = target
        if piece.node.shared_source_start is not None and piece.node_start == 0:
            for phrase in phrases:
                if phrase.first == piece.node.shared_source_start:
                    phrase_destination[phrase] = destination
                    break
        if piece.jump_destination is not None:
            jump = destination + len(payload)
            page[jump - PAGE_ADDRESS] = 0xFD
            distance = (jump + 3 - piece.jump_destination) & 0xFFFF
            put_word(page, jump - PAGE_ADDRESS + 1, distance)

    for source, (phrase, inner_offset) in interiors.items():
        source_map.setdefault(source, phrase_destination[phrase] + inner_offset)

    # Fill and patch every generated phrase caller.
    for piece in pieces:
        if piece.node.shared_source_start is not None:
            continue
        for index in range(piece.node_start, piece.node_end):
            source = piece.node.sources[index]
            if source is None or source_byte(engine, bank, source) == 0xF9:
                continue
            phrase = next((item for item in phrases
                           if source in (item.first, item.second)), None)
            if phrase is None:
                continue
            destination = piece.destination + index - piece.node_start
            distance = (destination + 3 - phrase_destination[phrase]) & 0xFFFF
            put_word(page, destination - PAGE_ADDRESS + 1, distance)

    # Relocate original relative stream-control operands. Shared phrase code
    # uses the first occurrence's private map; ordinary streams use callers.
    for piece in pieces:
        node = piece.node
        for index in range(piece.node_start, piece.node_end):
            source = node.sources[index]
            if source is None or source not in trace.boundaries:
                continue
            opcode = source_byte(engine, bank, source)
            length = CONTROL_LENGTHS.get(opcode)
            if length is None:
                continue
            # Generated F9 callers have no original operands to relocate.
            if source in {value for phrase in phrases
                          for value in (phrase.first, phrase.second)} and \
                    node.shared_source_start is None:
                continue
            target = stream_target(engine, bank, source)
            mapping = shared_map if node.shared_source_start is not None else source_map
            if target not in mapping:
                raise ValueError(
                    f"track {track}: stream target {target:04X} was not packed")
            command_destination = piece.destination + index - piece.node_start
            operand = CONTROL_OPERANDS[opcode]
            distance = (command_destination + length - mapping[target]) & 0xFFFF
            put_word(page, command_destination - PAGE_ADDRESS + operand, distance)

    amap = AddressMap()
    amap.add(MAIN_START, MAIN_END, MAIN_DEST)
    amap.add(WRAPPER_START, WRAPPER_END, WRAPPER_DEST)
    main_instructions = []
    for index, (start, end) in enumerate(trace.executed_ranges):
        start = max(start, MAIN_START)
        end = min(end, MAIN_END)
        if end > start:
            main_instructions.extend(disassemble_starts(
                image_slice(engine, start, end), start,
                f"snatcher-{track}-trace-{index}"))
    main_instructions = sorted(set(main_instructions))
    wrapper_instructions = disassemble_starts(
        image_slice(engine, WRAPPER_START, WRAPPER_END), WRAPPER_START,
        "snatcher-wrapper")
    patch_code(page, amap, MAIN_START, MAIN_DEST, MAIN_END - MAIN_START,
               main_instructions, PSG_GATEWAY)
    patch_code(page, amap, WRAPPER_START, WRAPPER_DEST,
               WRAPPER_END - WRAPPER_START, wrapper_instructions,
               PSG_GATEWAY)
    # D0-DF index entries 0-15; E0-FE index entries 16-46.  These are data
    # words consumed by JP (HL), so linear disassembly deliberately does not
    # see them as relocatable instruction operands.
    dispatch_destination = MAIN_DEST + COMMAND_DISPATCH_START - MAIN_START
    for index in range(COMMAND_DISPATCH_ENTRIES):
        offset = dispatch_destination - PAGE_ADDRESS + 2 * index
        # Read from the pristine source. Linear disassembly can decode a few
        # table bytes as instructions and may already have altered them.
        handler = source_word(
            engine, bank, COMMAND_DISPATCH_START + 2 * index)
        relocated = amap.get(handler)
        if relocated is None:
            raise ValueError(
                f"track {track}: command handler {handler:04X} is outside engine")
        put_word(page, offset, relocated)
    # The note dispatcher at 0813H selects a phrase through one of these two
    # absolute-pointer tables. Restore every entry from the pristine image as
    # well: linear disassembly can otherwise turn e.g. 1D16H into 1D54H.
    for table, entries in NOTE_POINTER_TABLES:
        destination = MAIN_DEST + table - MAIN_START
        for index in range(entries):
            handler = source_word(engine, bank, table + 2 * index)
            relocated = amap.get(handler)
            if relocated is None:
                raise ValueError(
                    f"track {track}: note phrase {handler:04X} is outside engine")
            put_word(page, destination - PAGE_ADDRESS + 2 * index, relocated)
    for table, entries in WAVE_POINTER_TABLES:
        destination = MAIN_DEST + table - MAIN_START
        for index in range(entries):
            wave = source_word(engine, bank, table + 2 * index)
            relocated = amap.get(wave)
            if relocated is None:
                raise ValueError(
                    f"track {track}: SCC waveform {wave:04X} is outside engine")
            put_word(page, destination - PAGE_ADDRESS + 2 * index, relocated)
    # The selected bank is already materialized; preserve OUT (FEH),A timing.
    bank_switch = MAIN_DEST + 0x0318 - MAIN_START
    if page[bank_switch - PAGE_ADDRESS:bank_switch - PAGE_ADDRESS + 2] != b"\xD3\xFE":
        raise ValueError("unexpected Snatcher bank-switch instruction")
    page[bank_switch - PAGE_ADDRESS:bank_switch - PAGE_ADDRESS + 2] = b"\x18\x00"

    # Song table points to a descriptor; its eight channel values are offsets
    # relative to the descriptor rather than absolute addresses.
    table_destination = MAIN_DEST + CFG.song_table + 2 * track - MAIN_START
    if descriptor not in source_map:
        raise ValueError(f"track {track}: descriptor {descriptor:04X} was not packed")
    descriptor_destination = source_map[descriptor]
    put_word(page, table_destination - PAGE_ADDRESS, descriptor_destination)
    for offset in range(2, 18, 2):
        relative = source_word(engine, bank, descriptor + offset)
        if relative == 0xFFFF:
            continue
        target = (descriptor + relative) & 0xFFFF
        # Disabled channels can retain descriptor offsets that are never
        # dereferenced and therefore do not occur in the full-duration trace.
        if target in source_map:
            put_word(page, descriptor_destination - PAGE_ADDRESS + offset,
                     source_map[target] - descriptor_destination)

    music_size = sum(len(node.data) for node in nodes)
    jump_bytes = 3 * sum(piece.jump_destination is not None for piece in pieces)
    return page, music_size + jump_bytes, sum(
        2 * phrase.length - (phrase.length + 1 + 6) for phrase in phrases)


def build(compressed: bool, destination: Path, packer: Path) -> None:
    source = (ROOT / "vigamup" / "snatcher.kss").read_bytes()
    load, load_size, init, play = struct.unpack_from("<HHHH", source, 4)
    if (source[:4] != b"KSCC" or
            (load, load_size, init, play) !=
            (CFG.load, CFG.load_size, CFG.init, CFG.play)):
        raise ValueError("unexpected Snatcher source KSS")
    engine = source[0x10:0x10 + load_size]
    banks = [source[0x10 + load_size + index * 0x4000:
                    0x10 + load_size + (index + 1) * 0x4000]
             for index in range(3)]
    rows = read_trackinfo(ROOT / "vigamup" / "snatcher.trackinfo")
    traces = read_traces(
        ROOT / "vigamup" / "extracted" / "snatcher.track_extract")
    pages = []
    for track, *_ in rows:
        page, music_size, saved = build_page(engine, banks, track, traces[track])
        pages.append(page)
        print(f"track {track}: packed music={music_size} phrase saving={saved}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    write_container("snatcher", CFG, rows, pages, compressed,
                    destination, packer)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uncompressed", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--zx0pack", type=Path,
                        default=ROOT / "build" / "zx0pack")
    args = parser.parse_args()
    suffix = ("complete_page.ksp" if args.uncompressed else
              "complete_page_compressed.ksp")
    destination = args.output or (ROOT / "vigamup" / "extracted" /
                                  f"snatcher_{suffix}")
    try:
        build(not args.uncompressed, destination, args.zx0pack)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
