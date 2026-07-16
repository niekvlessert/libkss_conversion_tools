#!/usr/bin/env python3
"""Convert the known F-1 Spirit KSCC image from 8K to 16K banking.

This is deliberately strict: it only patches the two verified bank-selection
sequences in the F-1 Spirit engine and refuses unexpected input images.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


HEADER_SIZE = 0x10
BANK_8K_SIZE = 0x2000
BANK_16K_SIZE = 0x4000
EXPECTED_LOAD_ADDRESS = 0x5F00
EXPECTED_BANK_OFFSET = 14
EXPECTED_BANK_COUNT = 2

OLD_SELECTOR = bytes.fromhex("3e 0e 32 00 90 3e 0f 32 00 b0")
NEW_SELECTOR = bytes.fromhex("3e 0e d3 fe 00 00 00 00 00 00")
EXPECTED_OFFSETS = (0x01, 0x81)


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def convert(source: Path, destination: Path) -> None:
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
        raise ValueError("input does not have F-1 Spirit's 2-bank 8K layout")
    if bank_offset != EXPECTED_BANK_OFFSET:
        raise ValueError(f"unexpected bank offset: {bank_offset}")

    expected_size = HEADER_SIZE + load_size + EXPECTED_BANK_COUNT * BANK_8K_SIZE
    if len(data) != expected_size:
        raise ValueError(
            f"unexpected file size: {len(data)} (expected {expected_size})"
        )

    engine_start = HEADER_SIZE
    engine_end = engine_start + load_size
    engine = bytearray(data[engine_start:engine_end])
    found_offsets = tuple(
        offset for offset in range(len(engine) - len(OLD_SELECTOR) + 1)
        if engine[offset : offset + len(OLD_SELECTOR)] == OLD_SELECTOR
    )
    if found_offsets != EXPECTED_OFFSETS:
        formatted = ", ".join(f"0x{offset:04X}" for offset in found_offsets)
        raise ValueError(
            f"unexpected selector sequences at [{formatted}]; refusing to patch"
        )

    for offset in found_offsets:
        engine[offset : offset + len(OLD_SELECTOR)] = NEW_SELECTOR

    bank_data = data[engine_end:]
    bank_0 = bank_data[:BANK_8K_SIZE]
    bank_1 = bank_data[BANK_8K_SIZE:]
    if len(bank_0) != BANK_8K_SIZE or len(bank_1) != BANK_8K_SIZE:
        raise ValueError("input bank data is not exactly two 8K banks")

    header = bytearray(data[:HEADER_SIZE])
    header[0x0D] = 1  # 16K mode, one bank; preserve bank_offset at 0x0C.
    result = bytes(header) + bytes(engine) + bank_0 + bank_1
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)

    print(f"patched {source} -> {destination}")
    print("  mapper: 8K x 2 -> 16K x 1")
    print(f"  bank offset: {bank_offset}")
    print("  patched engine offsets: 0x005F01, 0x005F81")
    print(f"  output size: {len(result)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source",
        nargs="?",
        type=Path,
        default=Path("vigamup/f1spirit.kss"),
    )
    parser.add_argument(
        "destination",
        nargs="?",
        type=Path,
        default=Path("vigamup/extracted/f1spirit_16k.kss"),
    )
    args = parser.parse_args()
    try:
        convert(args.source, args.destination)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
