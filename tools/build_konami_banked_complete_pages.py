#!/usr/bin/env python3
"""Build one-page KCPX/KCPZ images for banked Konami drivers.

F-1 Spirit and Nemesis 3 keep their engines at 5FxxH. Solid Snake and SD
Snatcher use game-specific page-1 relocations. Per-song bank bytes, writable
state, and the engine are materialized together in one 16K mapper page.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct

from build_konami_complete_pages import (
    CONFIGS, FIXED_SIZE, HEADER_SIZE, PAGE_SIZE, WRAPPER_DEST, Config, common_template,
    compress, descriptor_address, disassemble_starts, info_block, make_page,
    overlay_from_template, put_word, read_trackinfo,
)


ROOT = Path(__file__).resolve().parents[1]
SOURCE_HEADER_SIZE = 0x10

CONFIGS.update({
    "f1spirit": Config(6, "F-1 Spirit", 0x4000, 0x8000, 0x5F00, 0x5F80,
                       0x611E, 0x5F00, 0x8000, None,
                       0xE000, 0xE192, 0x4000,
                       True, 0x5F00, 0x5EF0),
    "nemesis3": Config(7, "Nemesis 3", 0x4000, 0x8000, 0x5FC0, 0x5FE0,
                       0x7504, 0x5FC0, 0x8000, None,
                       0xE000, 0xE300, 0x4000,
                       True, 0x5FC0, 0x5EF0,
                       ((0xFDA2, 0xFDA3, 0x4300),)),
    "solidsnake": Config(8, "Metal Gear 2: Solid Snake",
                         0x0070, 0xBFB4, 0xC017, 0xC020,
                         0x2619, 0x0070, 0x14F6, 0xC024,
                         0xC400, 0xC699, 0x4000,
                         True, 0x0070, 0x5F60,
                          ((0xFD9E, 0xFD9F, 0x42A0),
                           (0xCA5F, 0xCA60, 0x42A1),
                           (0xCB58, 0xCB59, 0x42A2)), 0x6000),
    "sdsnatch": Config(9, "SD Snatcher",
                        0x4000, 0x9EAD, 0xDE40, 0xDEA9,
                        0x6631, 0x43E0, 0x573B, 0xDEAD,
                        0x4000, 0x4300, 0x4000,
                        True, 0x43E0, 0x5F60, (), 0x43E0, 0xDE00, True),
})


def read_bank_traces(path: Path) -> tuple[
        dict[int, dict[int, list[tuple[int, int]]]],
        dict[int, tuple[int, int]], dict[int, list[tuple[int, int]]], set[int]]:
    ranges: dict[int, dict[int, list[tuple[int, int]]]] = {}
    combinations: dict[int, tuple[int, int]] = {}
    data_ranges: dict[int, list[tuple[int, int]]] = {}
    executed: set[int] = set()
    current = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("track="):
            current = int(line[6:])
            ranges[current] = {}
            data_ranges[current] = []
        elif current is not None and line.startswith("bank_data_read_ranges_"):
            left, value = line.split("=", 1)
            bank = int(left.rsplit("_", 1)[1])
            parsed = []
            if value != "none":
                for item in value.split(","):
                    start, end = item.split("-")
                    parsed.append((int(start, 16), int(end, 16)))
            ranges[current][bank] = parsed
        elif current is not None and line.startswith("page_combinations="):
            value = line.split("=", 1)[1]
            if value != "none":
                found = re.findall(r"\((\d+),(\d+)\)", value)
                if found:
                    left, right = found[-1]
                    combinations[current] = (int(left), int(right))
        elif current is not None and line.startswith("data_read_ranges="):
            value = line.split("=", 1)[1]
            if value != "none":
                for item in value.split(","):
                    start, end = item.split("-")
                    data_ranges[current].append((int(start, 16), int(end, 16)))
        elif current is not None and line.startswith("executed_all_ranges="):
            value = line.split("=", 1)[1]
            if value != "none":
                for item in value.split(","):
                    start, end = item.split("-")
                    executed.update(range(int(start, 16), int(end, 16)))
    return ranges, combinations, data_ranges, executed


def source_parts(stem: str) -> tuple[bytes, list[bytes]]:
    data = (ROOT / "vigamup" / f"{stem}.kss").read_bytes()
    load, size = struct.unpack_from("<HH", data, 4)
    descriptor = data[0x0D]
    bank_size = 0x2000 if descriptor & 0x80 else 0x4000
    bank_count = descriptor & 0x7F
    engine = data[SOURCE_HEADER_SIZE:SOURCE_HEADER_SIZE + size]
    banks = [data[SOURCE_HEADER_SIZE + size + i * bank_size:
                  SOURCE_HEADER_SIZE + size + (i + 1) * bank_size]
             for i in range(bank_count)]
    expected_load = {"f1spirit": 0x5F00, "nemesis3": 0x5FC0,
                     "solidsnake": 0x0070, "sdsnatch": 0x4300}[stem]
    if load != expected_load or any(len(bank) != bank_size for bank in banks):
        raise ValueError(f"unexpected {stem} source layout")
    return engine, banks


def virtual_image(stem: str, engine: bytes, banks: list[bytes],
                  combination: tuple[int, int]) -> bytes:
    if stem == "solidsnake":
        image = bytearray(engine)
        bank = banks[combination[0]]
        start = 0x8000 - 0x0070
        image[start:start + 0x4000] = bank
        return bytes(image)
    if stem == "sdsnatch":
        image = bytearray(0x9EAD)  # virtual 4000H..DEACH image
        image[0x0300:0x0300 + len(engine)] = engine
        image[0x4000:0x8000] = banks[combination[0]]
        return bytes(image)
    image = bytearray(0x8000)  # virtual 4000H..BFFFH image
    engine_address = 0x5F00 if stem == "f1spirit" else 0x5FC0
    image[engine_address - 0x4000:engine_address - 0x4000 + len(engine)] = engine
    left, right = combination
    image[0x4000:0x6000] = banks[left]
    image[0x6000:0x8000] = banks[right]
    return bytes(image)


def disable_original_bank_switches(page: bytearray, stem: str,
                                   executed: set[int]) -> None:
    base = -0x4000
    if stem == "f1spirit":
        for address in (0x5F01, 0x5F81):
            page[address + base:address + base + 10] = bytes(10)
    elif stem == "nemesis3":
        for address in (0x5FC1, 0x5FE1):
            page[address + base:address + base + 10] = bytes(10)
        for address in (0x62EC, 0x62F3, 0x62FA):
            page[address + base:address + base + 5] = bytes(5)
    elif stem == "solidsnake":
        page[0x6D3D + base:0x6D3F + base] = bytes(2)
        # The command dispatcher at 0DCAH is a word-aligned function-pointer
        # table embedded inside the otherwise contiguous engine block.
        for source in range(0x0DCA, 0x0E20, 2):
            address = 0x6000 + source - 0x0070
            target = struct.unpack_from("<H", page, address + base)[0]
            if target in executed and 0x0070 <= target < 0x14F6:
                put_word(page, address + base,
                         0x6000 + target - 0x0070)
    elif stem == "sdsnatch":
        # The selected bank is already materialized into the complete page.
        # Remove both OUT (FEH),A operations from the relocated KSS wrapper,
        # while retaining its song-number translation and initialization.
        for source in (0xDE43, 0xDE4F):
            offset = WRAPPER_DEST - 0x4000 + source - 0xDE00
            page[offset:offset + 2] = bytes(2)


def write_container(stem: str, cfg: Config, rows, pages, compressed: bool,
                    destination: Path, packer: Path) -> None:
    template = common_template(pages)
    overlays = [overlay_from_template(template, page) for page in pages]
    original_map = bytes([row[0] for row in rows] + [0xFF] * (256 - len(rows)))
    page_map = bytes(list(range(len(rows))) + [0xFF] * (256 - len(rows)))
    fixed = bytearray(b"KCPZ" if compressed else b"KCPX")
    fixed += bytes((1, len(pages), len(rows), cfg.variant))
    fixed += struct.pack("<HHHH", PAGE_SIZE, 0x4000, cfg.work_dest, 0)
    fixed += original_map + page_map
    assert len(fixed) == FIXED_SIZE

    if compressed:
        packed_template = compress(packer, f"{stem}-template", template)
        packed_overlays = [compress(packer, f"{stem}-overlay-{i}", overlay)
                           for i, overlay in enumerate(overlays)]
        tail = fixed + bytes(4 + len(pages) * 6)
        put_word(tail, FIXED_SIZE, len(packed_template))
        put_word(tail, FIXED_SIZE + 2, len(tail))
        tail += packed_template
        for index, (raw, packed) in enumerate(zip(overlays, packed_overlays)):
            record = FIXED_SIZE + 4 + index * 6
            put_word(tail, record, len(raw))
            put_word(tail, record + 2, len(packed))
            put_word(tail, record + 4, len(tail))
            tail += packed
    else:
        tail = fixed + template
        for overlay in overlays:
            tail += struct.pack("<H", len(overlay)) + overlay

    header = bytearray(HEADER_SIZE)
    header[:4] = b"KSSX"
    main_start = cfg.main_start if cfg.main_start is not None else cfg.load
    main_destination = (cfg.main_destination if cfg.main_destination is not None
                        else main_start)
    init = (WRAPPER_DEST + cfg.init - cfg.wrapper_start
            if cfg.wrapper_end is not None and
            cfg.wrapper_start <= cfg.init < cfg.wrapper_end
            else main_destination + cfg.init - main_start)
    play = (WRAPPER_DEST + cfg.play - cfg.wrapper_start
            if cfg.wrapper_end is not None and
            cfg.wrapper_start <= cfg.play < cfg.wrapper_end
            else main_destination + cfg.play - main_start)
    struct.pack_into("<HHHH", header, 4, 0x0200, 1, init, play)
    header[0x0D] = len(pages)
    header[0x0E] = 0x10
    put_word(header, 0x10, 1 + len(tail))
    put_word(header, 0x1A, len(rows) - 1)
    result = bytes(header) + b"\xC9" + bytes(tail) + info_block(cfg, rows)
    destination.write_bytes(result)
    destination.with_suffix(".trackinfo").write_text(
        "\n".join(f"{i},{title},{seconds},{fade},{'yes' if loop else 'no'}"
                  for i, (_, title, seconds, fade, loop) in enumerate(rows)) + "\n",
        encoding="utf-8")
    print(f"wrote {destination} ({len(result)} bytes, {len(rows)} tracks)")
    print(f"  template={len(template)}, overlays={sum(map(len, overlays))}")


def build(stem: str, compressed: bool, destination: Path, packer: Path) -> None:
    cfg = CONFIGS[stem]
    rows = read_trackinfo(ROOT / "vigamup" / f"{stem}.trackinfo")
    traces, combinations, data_ranges, executed = read_bank_traces(
        ROOT / "vigamup" / "extracted" / f"{stem}.track_extract")
    engine, banks = source_parts(stem)
    main_start = cfg.main_start or cfg.load
    source_load = {"f1spirit": 0x5F00, "nemesis3": 0x5FC0,
                   "solidsnake": 0x0070, "sdsnatch": 0x4300}[stem]
    main_offset = main_start - source_load
    instructions = disassemble_starts(
        engine[main_offset:main_offset + cfg.engine_end - main_start], main_start,
                                      f"{stem}-banked-main")
    wrapper_instructions = []
    if cfg.wrapper_end is not None:
        start = cfg.wrapper_start - source_load
        wrapper_instructions = disassemble_starts(
            engine[start:start + cfg.wrapper_end - cfg.wrapper_start],
            cfg.wrapper_start,
            f"{stem}-banked-wrapper")
    pages = []
    for track, *_ in rows:
        if stem == "f1spirit":
            combination = (0, 1)
        elif stem == "nemesis3":
            combination = combinations[track]
        else:
            active = [bank for bank, ranges in traces[track].items() if ranges]
            if not active:
                active = [0]
            if len(active) != 1:
                raise ValueError(f"track {track}: expected one active bank, got {active}")
            combination = (active[0], active[0])
        image = virtual_image(stem, engine, banks, combination)
        wanted = (list(data_ranges[track])
                  if stem in ("solidsnake", "sdsnatch") else [])
        if stem not in ("solidsnake", "sdsnatch"):
            for virtual_bank, source_bank in enumerate(combination):
                for start, end in traces[track].get(source_bank, []):
                    wanted.append((0x8000 + virtual_bank * 0x2000 + start,
                                   0x8000 + virtual_bank * 0x2000 + end))
        try:
            page = make_page(image, cfg, track, wanted,
                             max(row[0] for row in rows) + 1, {}, instructions,
                             wrapper_instructions)
        except ValueError as error:
            raise ValueError(f"track {track}: {error}") from error
        disable_original_bank_switches(page, stem, executed)
        pages.append(page)
    write_container(stem, cfg, rows, pages, compressed, destination, packer)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stem", choices=("f1spirit", "nemesis3", "solidsnake",
                                         "sdsnatch"))
    parser.add_argument("--uncompressed", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--zx0pack", type=Path, default=ROOT / "build" / "zx0pack")
    args = parser.parse_args()
    suffix = "complete_page.kss" if args.uncompressed else "complete_page_compressed.kss"
    destination = args.output or ROOT / "vigamup" / "extracted" / f"{args.stem}_{suffix}"
    try:
        build(args.stem, not args.uncompressed, destination, args.zx0pack)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
