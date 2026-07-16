#!/usr/bin/env python3
"""Repack Quarth's embedded music into 16K KSS mapper banks.

The original Quarth KSCC image loads the driver and all music at 4000H.  The
music starts at 52A4H, so part of it is below the 8000H mapper boundary and
the original image cannot simply be appended as a KSS bank.

This repacker keeps the executable driver at its original 4000H address,
relocates the small B800H helper/tail next to it, and builds two 16K banks.
Each bank starts with the shared Quarth lookup tables and then packs the track
data for one song group immediately after them.  The first required group is
larger than one 8K mapper half, so it cannot use a strict Nemesis-style
lower-half/upper-half split inside a 16K bank.  The init wrapper selects the
bank before the driver's normal track-selection routine runs.

The supported song set is read from Quarth's trackinfo file.  Other internal
sound-effect IDs are intentionally not packed; the input is rejected if a
trackinfo ID is outside the supported Quarth music set or its descriptor no
longer fits its assigned bank.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


HEADER_SIZE = 0x10
BANK_SIZE = 0x4000
BANK_BASE = 0x8000

LOAD_ADDRESS = 0x4000
CORE_END = 0x52A4
COMMON_START = 0x52A2
COMMON_END = 0x6573

TAIL_SOURCE_START = 0xB800
TAIL_HELPER_END = 0xB817

# Track 14's last stream starts at the same address as track 15's first
# stream.  Both self-contained banks therefore retain the overlapping stream
# bytes in their own packed range.
BANK_TRACKS = (
    tuple(range(3, 15)),
    tuple(range(15, 22)),
)
BANK_SOURCE_RANGES = (
    ((0x6573, 0x911C), (0xA0F0, 0xA270)),
    ((0x906D, 0xAF09),),
)

# The first range in each bank is the contiguous stream area for that bank's
# selected songs.  It is kept separate from the extra A0F0H dependency copy:
# bytes there are lookup data, not a stream to scan for command operands.
BANK_STREAM_RANGES = (
    ((0x6573, 0x911C),),
    ((0x906D, 0xAF09),),
)

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

# Absolute operands in the executable core which point into the embedded
# music/tables.  The listed address is the instruction address; the operand
# is the two bytes immediately after it.
ENGINE_MUSIC_REFERENCES = (
    (0x404B, bytes.fromhex("21 a2 52"), "track table"),
    (0x459F, bytes.fromhex("21 82 54"), "wave table 0"),
    (0x45A3, bytes.fromhex("21 9A 54"), "wave table 1"),
    (0x45AE, bytes.fromhex("21 B2 54"), "wave table 2a"),
    (0x45B2, bytes.fromhex("21 B2 54"), "wave table 2b"),
    (0x45BD, bytes.fromhex("21 CA 54"), "wave table 3a"),
    (0x45C1, bytes.fromhex("21 CA 54"), "wave table 3b"),
    (0x46B6, bytes.fromhex("21 C5 64"), "pitch table"),
    (0x487E, bytes.fromhex("21 2A 64"), "volume table"),
    (0x4DE9, bytes.fromhex("21 38 58"), "frequency table 0"),
    (0x4E55, bytes.fromhex("21 38 58"), "frequency table 1"),
)


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def put_u16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def image_slice(image: bytes, start: int, end: int) -> bytes:
    if start < LOAD_ADDRESS or end < start:
        raise ValueError(f"invalid image range 0x{start:04X}:0x{end:04X}")
    image_end = LOAD_ADDRESS + len(image)
    if end > image_end:
        raise ValueError(
            f"image range 0x{start:04X}:0x{end:04X} exceeds "
            f"0x{LOAD_ADDRESS:04X}:0x{image_end:04X}"
        )
    return image[start - LOAD_ADDRESS : end - LOAD_ADDRESS]


def source_word(image: bytes, address: int) -> int:
    return u16(image, address - LOAD_ADDRESS)


def common_address(address: int) -> int:
    if not COMMON_START <= address < COMMON_END:
        raise ValueError(f"0x{address:04X} is not in the common music block")
    return BANK_BASE + address - COMMON_START


def patch_common_word(common: bytearray, address: int, value: int) -> None:
    offset = address - COMMON_START
    if offset < 0 or offset + 2 > len(common):
        raise ValueError(f"common word outside relocated block: 0x{address:04X}")
    put_u16(common, offset, value)


def patch_engine_bytes(
    engine: bytearray, address: int, expected: bytes, replacement: bytes
) -> None:
    offset = address - LOAD_ADDRESS
    actual = bytes(engine[offset : offset + len(expected)])
    if actual != expected:
        raise ValueError(
            f"unexpected bytes at engine 0x{address:04X}: "
            f"{actual.hex(' ')} (expected {expected.hex(' ')})"
        )
    if len(expected) != len(replacement):
        raise ValueError("engine patch changes instruction width")
    engine[offset : offset + len(expected)] = replacement


def descriptor_address(image: bytes, track_id: int) -> int:
    table_entry = COMMON_START + 2 * track_id
    address = source_word(image, table_entry)
    if not COMMON_START <= address <= COMMON_END - 18:
        raise ValueError(
            f"track {track_id} descriptor 0x{address:04X} is not in "
            "the relocated common block"
        )
    return address


def next_descriptor_address(image: bytes, track_id: int) -> int:
    return descriptor_address(image, track_id + 1)


def read_trackinfo(path: Path) -> tuple[int, ...]:
    track_ids: list[int] = []
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            track_id = int(line.split(",", 1)[0].strip())
        except (ValueError, IndexError) as error:
            raise ValueError(f"invalid trackinfo line {line_number}: {line!r}") from error
        if not 0 <= track_id <= 255:
            raise ValueError(f"trackinfo ID out of range on line {line_number}: {track_id}")
        if track_id in track_ids:
            raise ValueError(f"duplicate trackinfo ID on line {line_number}: {track_id}")
        track_ids.append(track_id)
    if not track_ids:
        raise ValueError(f"trackinfo file is empty: {path}")
    return tuple(track_ids)


def make_common(image: bytes, track_ids: tuple[int, ...]) -> bytearray:
    common = bytearray(image_slice(image, COMMON_START, COMMON_END))

    # The driver indexes the compact song table as 52A2H + 2*song.  Only patch
    # entries named by trackinfo: after the real song entries this area
    # overlaps descriptors and lookup data, so it is not a 256-entry table.
    for track_id in track_ids:
        entry = COMMON_START + 2 * track_id
        old = source_word(image, entry)
        if not COMMON_START <= old < COMMON_END:
            raise ValueError(
                f"track {track_id} table entry is not a common descriptor: "
                f"0x{old:04X}"
            )
        patch_common_word(common, entry, common_address(old))

    # The four wave tables are pointer tables too.  Relocating only the
    # instruction operands that select them leaves their entries pointing
    # back into the original below-8000H image.
    for table_address, entry_count in WAVE_POINTER_TABLES:
        for index in range(entry_count):
            address = table_address + 2 * index
            old = source_word(image, address)
            if not COMMON_START <= old < COMMON_END:
                raise ValueError(
                    f"wave table entry {table_address:04X}[{index}] is not "
                    f"a common pointer: 0x{old:04X}"
                )
            patch_common_word(common, address, common_address(old))

    # 5838H is a 90-entry table of pointers into the common lookup data.  The
    # bytes after entry 59H are table data, not more pointers.
    for index in range(0x5A):
        address = 0x5838 + 2 * index
        old = source_word(image, address)
        if not COMMON_START <= old < COMMON_END:
            raise ValueError(
                f"frequency table entry {index} is not a common pointer: "
                f"0x{old:04X}"
            )
        patch_common_word(common, address, common_address(old))

    # 64C5H begins with one sentinel followed by twelve pointers.  The next
    # words are the pointed-to table data itself.
    if source_word(image, 0x64C5) != 0xFFC1:
        raise ValueError("unexpected 64C5H table sentinel")
    for index in range(1, 13):
        address = 0x64C5 + 2 * index
        old = source_word(image, address)
        if not COMMON_START <= old < COMMON_END:
            raise ValueError(
                f"pitch table entry {index} is not a common pointer: "
                f"0x{old:04X}"
            )
        patch_common_word(common, address, common_address(old))

    return common


def make_bank(
    image: bytes,
    common: bytearray,
    track_ids: tuple[int, ...],
    source_ranges: tuple[tuple[int, int], ...],
    stream_ranges: tuple[tuple[int, int], ...],
) -> bytes:
    bank = bytearray(BANK_SIZE)
    bank[: len(common)] = common
    data_offset = len(common)
    source_to_destination: list[tuple[int, int, int]] = []

    for start, end in source_ranges:
        chunk = image_slice(image, start, end)
        if data_offset + len(chunk) > BANK_SIZE:
            raise ValueError(
                f"bank overflows 16K: common=0x{len(common):04X}, "
                f"data end=0x{data_offset + len(chunk):04X}"
            )
        destination_start = BANK_BASE + data_offset
        bank[data_offset : data_offset + len(chunk)] = chunk
        source_to_destination.append((start, end, destination_start))
        data_offset += len(chunk)

    def relocate_data_pointer(address: int) -> int:
        for start, end, destination_start in source_to_destination:
            if start <= address < end:
                return destination_start + address - start
        raise ValueError(
            f"track data pointer 0x{address:04X} is not present in "
            f"bank ranges {source_ranges}"
        )

    def relocate_pointer(address: int) -> int:
        if address in (0xFFFF, 0xFFCF):
            return address
        if COMMON_START <= address < COMMON_END:
            return common_address(address)
        return relocate_data_pointer(address)

    # 642AH is an eight-entry pointer table.  Entry 0 points into the music
    # area (A0F0H); the remaining entries point into the common lookup block.
    for index, expected in enumerate(VOLUME_TABLE_POINTERS):
        address = VOLUME_TABLE_ADDRESS + 2 * index
        old = source_word(image, address)
        if old != expected:
            raise ValueError(
                f"unexpected volume table entry {index}: "
                f"0x{old:04X} (expected 0x{expected:04X})"
            )
        patch_common_word(bank, address, relocate_pointer(old))

    # Patch the descriptor stream pointers only for songs assigned to this
    # bank.  Descriptors are variable length: their end is the next
    # descriptor in the compact table, not a fixed eight-pointer record.
    # The shared common block is repeated in each physical 16K bank, while
    # the remainder contains that bank's track data.
    for track_id in track_ids:
        descriptor = descriptor_address(image, track_id)
        descriptor_end = next_descriptor_address(image, track_id)
        if descriptor_end <= descriptor + 2 or (descriptor_end - descriptor) % 2:
            raise ValueError(
                f"invalid descriptor bounds for track {track_id}: "
                f"0x{descriptor:04X}:0x{descriptor_end:04X}"
            )
        for field in range(descriptor + 2, descriptor_end, 2):
            old = source_word(image, field)
            patch_common_word(bank, field, relocate_pointer(old))

    # F9, FC, and FD are stream commands with a little-endian absolute target
    # immediately after the opcode.  The target is still in the original
    # below-8000H image unless each operand is relocated as well.  Scan in
    # address order and mark operands as consumed so an F9/FC/FD byte inside a
    # preceding pointer operand is not mistaken for another command.
    operand_bytes: set[int] = set()
    for start, end in stream_ranges:
        for address in range(start, end - 2):
            if address in operand_bytes:
                continue
            opcode = image[address - LOAD_ADDRESS]
            if opcode not in (0xF9, 0xFC, 0xFD):
                continue
            target = source_word(image, address + 1)
            if not any(
                stream_start <= target < stream_end
                for stream_start, stream_end in stream_ranges
            ):
                continue
            replacement = relocate_pointer(target)
            destination_offset = address - LOAD_ADDRESS
            for source_start, source_end, destination_start in source_to_destination:
                if source_start <= address < source_end:
                    destination_offset = (
                        destination_start - BANK_BASE + address - source_start
                    )
                    break
            else:
                raise ValueError(
                    f"stream command 0x{opcode:02X} at 0x{address:04X} "
                    "is outside the packed bank ranges"
                )
            put_u16(bank, destination_offset + 1, replacement)
            operand_bytes.update((address + 1, address + 2))

    return bytes(bank)


def relative_jump(origin: int, target: int) -> int:
    displacement = target - (origin + 2)
    if not -128 <= displacement <= 127:
        raise ValueError(
            f"relative jump out of range: 0x{origin:04X} -> 0x{target:04X}"
        )
    return displacement & 0xFF


def make_tail(image: bytes) -> tuple[bytes, int, int, int]:
    """Return relocated tail bytes and helper/init/play addresses."""
    helper = bytearray(image_slice(image, TAIL_SOURCE_START, TAIL_HELPER_END))
    tail_start = CORE_END
    helper_address = tail_start
    init_address = helper_address + len(helper)

    # The init wrapper first gives the original init routine a known bank,
    # then selects the song's bank, and finally calls the original selector
    # with the original A value restored.
    selector_address = init_address + 19
    init = bytearray(
        (
            0xF5,  # PUSH AF: preserve the original init argument
            0xF5,  # PUSH AF: preserve it while selecting the common bank
            0xAF,  # XOR A
            0xD3,
            0xFE,  # OUT (FEH),A: select bank 0 for original INIT
            0xF1,  # POP AF: original init sees the original song argument
            0xCD,
            0x00,
            0x40,  # CALL 4000H
            0xF1,  # POP AF
            0xF5,  # PUSH AF while selector changes A
            0xCD,
            selector_address & 0xFF,
            selector_address >> 8,  # CALL selector
            0xF1,  # POP AF: restore song ID
            0xCD,
            0x03,
            0x40,  # CALL 4003H
            0xC9,  # RET
        )
    )

    if len(init) != 19:
        raise AssertionError("unexpected Quarth init wrapper size")

    # Use a compact selector with two compares:
    #   A < 15       -> bank 0
    #   A < 22       -> bank 1
    #   otherwise    -> bank 0
    # The selector is rebuilt here to make the boundaries explicit.
    bank0_address = selector_address + 8
    bank1_address = bank0_address + 4
    selector = bytearray(
        (
            0xFE,
            0x0F,  # CP 15
            0x38,
            relative_jump(selector_address + 2, bank0_address),
            0xFE,
            0x16,  # CP 22
            0x38,
            relative_jump(selector_address + 6, bank1_address),
            0xAF,
            0xD3,
            0xFE,
            0xC9,  # bank 0 for IDs >=22
            0x3E,
            0x01,
            0xD3,
            0xFE,
            0xC9,  # bank 1 for IDs 15..21
        )
    )
    if len(selector) != 17:
        raise AssertionError("unexpected Quarth bank selector size")

    play_address = init_address + len(init) + len(selector)
    play = bytes((0xCD, 0x06, 0x40, 0xC9))

    tail = bytes(helper) + bytes(init) + bytes(selector) + play
    return tail, helper_address, init_address, play_address


def repack(source: Path, destination: Path, trackinfo: Path) -> None:
    data = source.read_bytes()
    if len(data) < HEADER_SIZE or data[:4] != b"KSCC":
        raise ValueError("input is not a KSCC file")

    load_address = u16(data, 0x04)
    load_size = u16(data, 0x06)
    init_address = u16(data, 0x08)
    play_address = u16(data, 0x0A)
    bank_descriptor = data[0x0D]

    if (load_address, load_size, init_address, play_address) != (
        0x4000,
        0x7824,
        0xB817,
        0xB820,
    ):
        raise ValueError("input does not match the original Quarth KSCC layout")
    if bank_descriptor != 0:
        raise ValueError("input Quarth image unexpectedly already has banks")
    expected_size = HEADER_SIZE + load_size
    if len(data) != expected_size:
        raise ValueError(
            f"unexpected input size {len(data)} (expected {expected_size})"
        )

    track_ids = read_trackinfo(trackinfo)
    supported_track_ids = {track_id for group in BANK_TRACKS for track_id in group}
    unsupported = sorted(set(track_ids) - supported_track_ids)
    if unsupported:
        raise ValueError(
            f"trackinfo contains unsupported Quarth IDs: "
            f"{', '.join(str(track_id) for track_id in unsupported)}"
        )

    image = data[HEADER_SIZE : HEADER_SIZE + load_size]
    engine = bytearray(image_slice(image, LOAD_ADDRESS, CORE_END))

    tail, helper_address, new_init, new_play = make_tail(image)
    if helper_address != CORE_END:
        raise AssertionError("Quarth helper was not relocated after the core")

    # All direct engine references into the old music block now point into the
    # common 8000H bank window.
    music_targets = {
        0x52A2: BANK_BASE,
        0x5482: common_address(0x5482),
        0x549A: common_address(0x549A),
        0x54B2: common_address(0x54B2),
        0x54CA: common_address(0x54CA),
        0x64C5: common_address(0x64C5),
        0x642A: common_address(0x642A),
        0x5838: common_address(0x5838),
        helper_address: helper_address,
    }
    for address, expected, label in ENGINE_MUSIC_REFERENCES:
        old_target = u16(expected, 1)
        replacement_target = music_targets[old_target]
        replacement = expected[:1] + struct.pack("<H", replacement_target)
        patch_engine_bytes(engine, address, expected, replacement)
        print(f"  patched {label}: 0x{old_target:04X} -> 0x{replacement_target:04X}")

    # The B800H helper is called from the play routine, and its target must be
    # changed after the helper is moved next to the executable core.
    patch_engine_bytes(
        engine,
        0x4F3F,
        bytes.fromhex("CD 00 B8"),
        bytes((0xCD, helper_address & 0xFF, helper_address >> 8)),
    )

    common = make_common(image, track_ids)
    bank_tracks = tuple(
        tuple(track_id for track_id in group if track_id in track_ids)
        for group in BANK_TRACKS
    )
    banks = [
        make_bank(image, common, group, ranges, stream_ranges)
        for group, ranges, stream_ranges in zip(
            bank_tracks, BANK_SOURCE_RANGES, BANK_STREAM_RANGES
        )
    ]
    for index, bank in enumerate(banks):
        if len(bank) != BANK_SIZE:
            raise AssertionError(f"bank {index} is not exactly 16K")

    main_image = bytes(engine) + tail
    new_load_size = len(main_image)
    if LOAD_ADDRESS + new_load_size > 0x8000:
        raise ValueError("relocated main image no longer fits below 8000H")

    header = bytearray(data[:HEADER_SIZE])
    put_u16(header, 0x04, LOAD_ADDRESS)
    put_u16(header, 0x06, new_load_size)
    put_u16(header, 0x08, new_init)
    put_u16(header, 0x0A, new_play)
    header[0x0C] = 0
    header[0x0D] = len(banks)  # 16K mode, bank selectors 0..1.

    result = bytes(header) + main_image + b"".join(banks)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)

    print(f"repacked {source} -> {destination}")
    print(f"  main image: 0x{LOAD_ADDRESS:04X}:0x{LOAD_ADDRESS + new_load_size:04X}")
    print(f"  init/play: 0x{new_init:04X}/0x{new_play:04X}")
    print(f"  mapper: {len(banks)} x 16K banks at 0x{BANK_BASE:04X}")
    for index, (group, ranges) in enumerate(zip(bank_tracks, BANK_SOURCE_RANGES)):
        print(
            f"  bank {index}: tracks {group[0]}..{group[-1]}, "
            f"source ranges {ranges}, size {len(banks[index])} bytes"
        )
    print(f"  trackinfo: {trackinfo} ({len(track_ids)} tracks)")
    print(f"  output size: {len(result)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source", nargs="?", type=Path, default=Path("vigamup/quarth.kss")
    )
    parser.add_argument(
        "destination",
        nargs="?",
        type=Path,
        default=Path("vigamup/extracted/quarth_16k.kss"),
    )
    parser.add_argument(
        "--trackinfo",
        type=Path,
        default=Path("vigamup/quarth.trackinfo"),
        help="Quarth trackinfo file defining the required song IDs",
    )
    args = parser.parse_args()
    try:
        repack(args.source, args.destination, args.trackinfo)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
