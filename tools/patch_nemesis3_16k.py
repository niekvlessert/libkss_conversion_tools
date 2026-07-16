#!/usr/bin/env python3
"""Convert Nemesis 3 from 8K to 16K banking for real trackinfo tracks.

The real track set is taken from trackinfo and cross-checked against the
runtime page-combination analysis. Only combinations used by those tracks
are packed into the output.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct


HEADER_SIZE = 0x10
BANK_8K_SIZE = 0x2000
EXPECTED_LOAD_ADDRESS = 0x5FC0
EXPECTED_BANK_OFFSET = 9
EXPECTED_BANK_COUNT = 5

OLD_INIT_SELECTOR = bytes.fromhex("3e 0b 32 00 90 3e 0c 32 00 b0")

# Original page-5 values 0x0A, 0x0D, and 0x09 select source pages 1, 4,
# and 0 respectively. They are converted only when used by real tracks.
DYNAMIC_PATCHES = {
    0x32C: (1, bytes.fromhex("3e 0a 32 00 b0")),
    0x333: (4, bytes.fromhex("3e 0d 32 00 b0")),
    0x33A: (0, bytes.fromhex("3e 09 32 00 b0")),
}

REAL_COMBINATIONS = ((2, 3), (2, 4), (2, 0))


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def patch_at(engine: bytearray, offset: int, old: bytes, new: bytes) -> None:
    if engine[offset : offset + len(old)] != old:
        actual = engine[offset : offset + len(old)].hex(" ")
        raise ValueError(
            f"unexpected bytes at engine offset 0x{offset:04X}: {actual}"
        )
    if len(old) != len(new):
        raise ValueError("patch changes instruction width")
    engine[offset : offset + len(old)] = new


def read_real_track_ids(path: Path) -> set[int]:
    ids = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"\s*(\d+)\s*,", line)
        if match:
            ids.add(int(match.group(1)))
    if not ids:
        raise ValueError(f"no real track IDs found in {path}")
    return ids


def read_analyzed_combinations(path: Path) -> dict[int, set[tuple[int, int]]]:
    tracks: dict[int, set[tuple[int, int]]] = {}
    current = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("track="):
            current = int(line.split("=", 1)[1])
            tracks[current] = set()
        elif current is not None and line.startswith("page_combinations="):
            tracks[current].update(
                (int(left), int(right))
                for left, right in re.findall(r"\((\d+),(\d+)\)", line)
            )
    if not tracks:
        raise ValueError(f"no analyzed tracks found in {path}")
    return tracks


def determine_combinations(
    trackinfo: Path, analysis: Path
) -> tuple[set[int], tuple[tuple[int, int], ...]]:
    real_ids = read_real_track_ids(trackinfo)
    analyzed = read_analyzed_combinations(analysis)
    missing = sorted(real_ids - analyzed.keys())
    if missing:
        raise ValueError(f"analysis is missing trackinfo IDs: {missing}")
    combinations = set()
    for track_id in real_ids:
        combinations.update(analyzed[track_id])
    if combinations != set(REAL_COMBINATIONS):
        raise ValueError(
            f"unexpected real-track combinations: {sorted(combinations)}"
        )
    return real_ids, REAL_COMBINATIONS


def convert(source: Path, destination: Path, trackinfo: Path, analysis: Path) -> None:
    data = source.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError("input is shorter than a KSS header")
    if data[:4] != b"KSCC":
        raise ValueError("input is not a KSCC file")

    load_address = word(data, 0x04)
    load_size = word(data, 0x06)
    bank_offset = data[0x0C]
    bank_descriptor = data[0x0D]
    bank_count = bank_descriptor & 0x7F
    is_8k = bool(bank_descriptor & 0x80)

    if load_address != EXPECTED_LOAD_ADDRESS:
        raise ValueError(f"unexpected load address: 0x{load_address:04X}")
    if not is_8k or bank_count != EXPECTED_BANK_COUNT:
        raise ValueError("input does not have Nemesis 3's 5-bank 8K layout")
    if bank_offset != EXPECTED_BANK_OFFSET:
        raise ValueError(f"unexpected bank offset: {bank_offset}")

    expected_size = HEADER_SIZE + load_size + EXPECTED_BANK_COUNT * BANK_8K_SIZE
    if len(data) != expected_size:
        raise ValueError(
            f"unexpected file size: {len(data)} (expected {expected_size})"
        )

    real_track_ids, bank_combinations = determine_combinations(trackinfo, analysis)
    engine_start = HEADER_SIZE
    engine_end = engine_start + load_size
    engine = bytearray(data[engine_start:engine_end])
    init_offsets = tuple(
        offset for offset in range(len(engine) - len(OLD_INIT_SELECTOR) + 1)
        if engine[offset : offset + len(OLD_INIT_SELECTOR)] == OLD_INIT_SELECTOR
    )
    if init_offsets != (0x01, 0x21):
        formatted = ", ".join(f"0x{offset:04X}" for offset in init_offsets)
        raise ValueError(
            f"unexpected initial selectors at [{formatted}]; refusing to patch"
        )

    bank_selectors = {
        combination: bank_offset + index
        for index, combination in enumerate(bank_combinations)
    }
    initial_selector = bank_selectors[(2, 3)]
    new_init_selector = bytes([0x3E, initial_selector, 0xD3, 0xFE]) + bytes(6)
    for offset in init_offsets:
        patch_at(engine, offset, OLD_INIT_SELECTOR, new_init_selector)

    for offset, (source_page, old) in DYNAMIC_PATCHES.items():
        combination = (2, source_page)
        if combination in bank_selectors:
            selector = bank_selectors[combination]
            new = bytes([0x3E, selector, 0xD3, 0xFE, 0x00])
            patch_at(engine, offset, old, new)
        elif engine[offset : offset + len(old)] != old:
            raise ValueError(f"unexpected bytes at engine offset 0x{offset:04X}")

    source_banks = [
        data[
            engine_end + index * BANK_8K_SIZE :
            engine_end + (index + 1) * BANK_8K_SIZE
        ]
        for index in range(EXPECTED_BANK_COUNT)
    ]
    if any(len(bank) != BANK_8K_SIZE for bank in source_banks):
        raise ValueError("input bank data is not exactly five 8K banks")

    output_banks = b"".join(
        source_banks[left] + source_banks[right]
        for left, right in bank_combinations
    )
    header = bytearray(data[:HEADER_SIZE])
    header[0x0D] = len(bank_combinations)  # 16K mode, real-track banks only.
    result = bytes(header) + bytes(engine) + output_banks
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)

    print(f"patched {source} -> {destination}")
    print(f"  mapper: 5x8K -> {len(bank_combinations)}x16K")
    print(f"  bank offset: {bank_offset}")
    print(f"  trackinfo tracks: {len(real_track_ids)}")
    print("  real-track combinations: " + ", ".join(map(str, bank_combinations)))
    print("  unused (2,1) branch is intentionally not packed")
    print(f"  output size: {len(result)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source",
        nargs="?",
        type=Path,
        default=Path("vigamup/nemesis3.kss"),
    )
    parser.add_argument(
        "destination",
        nargs="?",
        type=Path,
        default=Path("vigamup/extracted/nemesis3_16k.kss"),
    )
    parser.add_argument(
        "--trackinfo",
        type=Path,
        default=Path("vigamup/nemesis3.trackinfo"),
    )
    parser.add_argument(
        "--analysis",
        type=Path,
        default=Path("vigamup/extracted/nemesis3.track_extract"),
    )
    args = parser.parse_args()
    try:
        convert(args.source, args.destination, args.trackinfo, args.analysis)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
