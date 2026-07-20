#!/usr/bin/env python3
"""Build a full Haunted Castle KSP development image.

The engine remains address-compatible with the original 768e01.e4 Z80 ROM.
YM3812 register writes are redirected to the default MoonSound OPL3/OPL2
ports C4/C5. K007232 accesses are redirected to a virtual register gateway
on I/O ports D0/D1; a desktop player can emulate or translate that device,
and a later native MSX runtime can translate the same register protocol to
OPL4 PCM voices. The original 512 KiB K007232 ROM is stored as K7RM chunk 0.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

AUDIO_SHA1 = "c55f468c0da6afdaa2af65a111583c0c42868bd1"
PCM_SHA1 = "01252d2ce7b14cfbe39ac8d7a5bd7417f1c2fc22"

WRAPPER_BASE = 0x8800
SONG_WINDOW = 0x8F00
LOAD_SIZE = 0x9000

KSP_ENGINE_TYPE_NATIVE_Z80 = 2
KSP_FLAG_SCC = 1 << 0
KSP_FLAG_MOONSOUND_FM = 1 << 1
KSP_FLAG_VIRTUAL_K007232 = 1 << 2
KSP_FLAG_K007232_ROM = 1 << 3
KSP_FLAG_OPL2_MODE = 1 << 4
KSP_FLAG_NTSC_60HZ = 1 << 5
KSP_FLAGS = (
    KSP_FLAG_SCC
    | KSP_FLAG_MOONSOUND_FM
    | KSP_FLAG_VIRTUAL_K007232
    | KSP_FLAG_K007232_ROM
    | KSP_FLAG_OPL2_MODE
    | KSP_FLAG_NTSC_60HZ
)

MOONSOUND_FM_ADDR = 0xC4
MOONSOUND_FM_DATA = 0xC5
MOONSOUND_FM_BANK1_ADDR = 0xC6
MOONSOUND_FM_BANK1_DATA = 0xC7
K007_REG_PORT = 0xD0
K007_DATA_PORT = 0xD1


class Asm:
    def __init__(self, origin: int):
        self.origin = origin
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.rel8: list[tuple[int, str]] = []
        self.abs16: list[tuple[int, str]] = []

    @property
    def pc(self) -> int:
        return self.origin + len(self.code)

    def label(self, name: str) -> None:
        if name in self.labels:
            raise ValueError(f"duplicate label {name}")
        self.labels[name] = self.pc

    def emit(self, *values: int) -> None:
        self.code.extend(v & 0xFF for v in values)

    def word(self, value: int) -> None:
        self.emit(value, value >> 8)

    def call(self, address: int) -> None:
        self.emit(0xCD)
        self.word(address)

    def call_label(self, name: str) -> None:
        self.emit(0xCD, 0, 0)
        self.abs16.append((len(self.code) - 2, name))

    def jp_label(self, name: str) -> None:
        self.emit(0xC3, 0, 0)
        self.abs16.append((len(self.code) - 2, name))

    def jr(self, opcode: int, name: str) -> None:
        self.emit(opcode, 0)
        self.rel8.append((len(self.code) - 1, name))

    def resolve(self) -> bytes:
        for pos, name in self.abs16:
            address = self.labels[name]
            self.code[pos:pos + 2] = struct.pack("<H", address)
        for pos, name in self.rel8:
            target = self.labels[name]
            after = self.origin + pos + 1
            displacement = target - after
            if not -128 <= displacement <= 127:
                raise ValueError(f"relative branch to {name} is out of range")
            self.code[pos] = displacement & 0xFF
        return bytes(self.code)


@dataclass
class Chunk:
    type: bytes
    id: int
    data: bytes
    aux: int = 0

    def __post_init__(self) -> None:
        if len(self.type) != 4:
            raise ValueError("chunk type must contain exactly four bytes")


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def patch_exact(image: bytearray, offset: int, expected: bytes, replacement: bytes) -> None:
    actual = bytes(image[offset:offset + len(expected)])
    if actual != expected:
        raise ValueError(
            f"unexpected source bytes at {offset:04X}: {actual.hex()} != {expected.hex()}"
        )
    if len(replacement) > len(expected):
        raise ValueError("replacement is larger than patch site")
    image[offset:offset + len(expected)] = replacement + bytes(len(expected) - len(replacement))


def build_wrapper() -> tuple[bytes, dict[str, int]]:
    a = Asm(WRAPPER_BASE)

    # INIT: KSP SONG byte wins. Standalone KSS falls back to input A when the
    # byte at SONG_WINDOW is FFH.
    a.label("init")
    a.emit(0x5F)                          # LD E,A
    a.emit(0x3A); a.word(SONG_WINDOW)     # LD A,(SONG_WINDOW)
    a.emit(0xFE, 0x15)                    # CP 21
    a.jr(0x38, "song_ready")              # JR C
    a.emit(0x7B)                          # LD A,E
    a.emit(0xFE, 0x15)
    a.jr(0x38, "song_ready")
    a.emit(0xAF)                          # XOR A
    a.label("song_ready")
    a.emit(0x32, 0, 0)                    # LD (selected_song),A
    selected_store = len(a.code) - 2
    a.call_label("moonsound_init")       # configure OPL4 FM as OPL2
    a.call(0x02FD)                        # original initialization
    a.emit(0x3A, 0, 0)                    # LD A,(selected_song)
    selected_load = len(a.code) - 2
    a.emit(0xC6, 0x50)                    # ADD A,50H
    a.call(0x003C)                        # original command dispatcher
    a.emit(0xC9)

    a.label("play")
    a.call(0x0476)                        # one complete channel pass
    a.emit(0xC9)

    a.label("stop")
    a.emit(0xAF)                          # XOR A
    a.emit(0x32); a.word(0x988F)          # SCC key mask = 0
    a.emit(0x4F)                          # LD C,A
    a.emit(0x06, 0xB0)                    # LD B,B0H
    a.label("stop_opl_loop")
    a.call(0x0F06)                        # patched MoonSound OPL2 writer
    a.emit(0x04)                          # INC B
    a.emit(0x78)                          # LD A,B
    a.emit(0xFE, 0xB9)                    # CP B9H
    a.jr(0x20, "stop_opl_loop")           # JR NZ
    a.emit(0xAF)                          # XOR A
    a.call_label("k7_write_0d_a")
    a.emit(0xC9)

    # Configure the MoonSound/YMF278B FM block directly from the KSP engine.
    # Register-array-1 register 05H:
    #   NEW=0  -> OPL2-compatible FM mode
    #   NEW2=1 -> keep OPL4 PCM register access available for the later
    #             K007232-to-OPL4 PCM backend.
    a.label("moonsound_init")
    a.emit(0x3E, 0x05)                    # LD A,05H
    a.emit(0xD3, MOONSOUND_FM_BANK1_ADDR) # OUT (C6H),A
    a.emit(0x06, 0x06)                    # LD B,6
    a.label("moonsound_init_wait1")
    a.emit(0x10, 0xFE)                    # DJNZ $
    a.emit(0x3E, 0x02)                    # NEW2=1, NEW=0
    a.emit(0xD3, MOONSOUND_FM_BANK1_DATA) # OUT (C7H),A
    a.emit(0x06, 0x06)
    a.label("moonsound_init_wait2")
    a.emit(0x10, 0xFE)
    a.emit(0xC9)

    # Virtual K007232 register protocol:
    #   OUT (D0H),register
    #   OUT (D1H),value
    # Reads select the register on D0H and IN from D1H.
    a.label("k7_write_bc")
    a.emit(0x78, 0xD3, K007_REG_PORT)      # LD A,B / OUT (D0),A
    a.emit(0x79, 0xD3, K007_DATA_PORT)     # LD A,C / OUT (D1),A
    a.emit(0xC9)

    a.label("k7_read_b")
    a.emit(0x78, 0xD3, K007_REG_PORT)
    a.emit(0xDB, K007_DATA_PORT, 0xC9)     # IN A,(D1) / RET

    for label, register in (
        ("k7_write_0c_a", 0x0C),
        ("k7_write_0d_a", 0x0D),
    ):
        a.label(label)
        a.emit(0xC5)                      # PUSH BC
        a.emit(0x4F)                      # LD C,A
        a.emit(0x06, register)             # LD B,register
        a.call_label("k7_write_bc")
        a.emit(0xC1, 0xC9)                # POP BC / RET

    for label, register in (
        ("k7_read_05", 0x05),
        ("k7_read_0b", 0x0B),
    ):
        a.label(label)
        a.emit(0xC5)                      # PUSH BC
        a.emit(0x06, register)
        a.call_label("k7_read_b")
        a.emit(0xC1, 0xC9)

    # Full replacements for the original memory-mapped K007232 helpers.
    a.label("k7_start")
    a.emit(0xC5)                          # PUSH BC: original B,C
    a.emit(0xDD, 0x7E, 0x30, 0xFE, 0x04) # LD A,(IX+30H) / CP 4
    a.emit(0x06, 0x02)                    # channel 0 base register 2
    a.jr(0x28, "k7_start_base_ready")
    a.emit(0x06, 0x08)                    # channel 1 base register 8
    a.label("k7_start_base_ready")
    a.emit(0x4B)                          # LD C,E
    a.call_label("k7_write_bc")
    a.emit(0x04, 0x4A)                    # INC B / LD C,D
    a.call_label("k7_write_bc")
    a.emit(0x04, 0xD1, 0x4B)              # INC B / POP DE / LD C,E
    a.call_label("k7_write_bc")
    a.emit(0x78, 0xFE, 0x04)              # B is now 4 or 0AH
    a.jr(0x20, "k7_start_ch1_bank")
    a.emit(0x3A, 0x17, 0x83, 0xE6, 0x0C, 0xB2)
    a.jr(0x18, "k7_start_bank_ready")
    a.label("k7_start_ch1_bank")
    a.emit(0x3A, 0x17, 0x83, 0xE6, 0x03, 0x4A)
    a.emit(0xCB, 0x21, 0xCB, 0x21, 0xB1)
    a.label("k7_start_bank_ready")
    a.emit(0x32, 0x17, 0x83, 0x4F, 0x06, 0x10)
    a.call_label("k7_write_bc")
    a.emit(0xC9)

    a.label("k7_frequency")
    a.emit(0xDD, 0x7E, 0x30, 0xFE, 0x04, 0x06, 0x00)
    a.jr(0x28, "k7_freq_base_ready")
    a.emit(0x06, 0x06)
    a.label("k7_freq_base_ready")
    a.emit(0x4B)
    a.call_label("k7_write_bc")
    a.emit(0x04, 0x4A)
    a.jp_label("k7_write_bc")             # helper RET returns to caller

    a.label("k7_volume")
    a.emit(0xDD, 0x7E, 0x30, 0xFE, 0x04)
    a.jr(0x20, "k7_volume_ch1")
    a.emit(0x3A, 0x15, 0x83, 0xE6, 0x0F)
    a.emit(0xCB, 0x21, 0xCB, 0x21, 0xCB, 0x21, 0xCB, 0x21, 0xB1)
    a.jr(0x18, "k7_volume_ready")
    a.label("k7_volume_ch1")
    a.emit(0x3A, 0x15, 0x83, 0xE6, 0xF0, 0xB1)
    a.label("k7_volume_ready")
    a.emit(0x32, 0x15, 0x83, 0x4F, 0x06, 0x0C)
    a.call_label("k7_write_bc")
    a.jp_label("k7_write_bc")             # original driver wrote twice

    a.label("k7_trigger")
    a.emit(0xDD, 0x7E, 0x30, 0xFE, 0x04, 0x06, 0x05)
    a.jr(0x28, "k7_trigger_reg_ready")
    a.emit(0x06, 0x0B)
    a.label("k7_trigger_reg_ready")
    a.call_label("k7_read_b")
    a.jp_label("k7_read_b")               # original driver read twice

    a.label("k7_channel_stop")
    a.emit(0xDD, 0x7E, 0x30, 0xFE, 0x04)
    a.jr(0x20, "k7_stop_ch1")
    a.emit(0x3A, 0x16, 0x83, 0xE6, 0x02)
    a.jr(0x18, "k7_stop_ready")
    a.label("k7_stop_ch1")
    a.emit(0x3A, 0x16, 0x83, 0xE6, 0x01)
    a.label("k7_stop_ready")
    a.emit(0x32, 0x16, 0x83, 0x4F, 0x06, 0x0D)
    a.jp_label("k7_write_bc")

    a.label("selected_song")
    a.emit(0)

    a.label("capability")
    a.emit(*b"HCAP")
    a.emit(1, 0)                          # version 1
    a.emit(MOONSOUND_FM_ADDR, MOONSOUND_FM_DATA)
    a.emit(MOONSOUND_FM_BANK1_ADDR, MOONSOUND_FM_BANK1_DATA)
    a.emit(K007_REG_PORT, K007_DATA_PORT)
    a.emit(*struct.pack("<I", KSP_FLAGS))
    a.emit(*b"K7RM")
    a.emit(*struct.pack("<I", 0))

    code = bytearray(a.resolve())
    selected = a.labels["selected_song"]
    code[selected_store:selected_store + 2] = struct.pack("<H", selected)
    code[selected_load:selected_load + 2] = struct.pack("<H", selected)
    return bytes(code), a.labels


def build_k007_start_patch(helper: int) -> bytes:
    """Replacement for 11A6-11DB, exactly 54 bytes."""
    a = Asm(0x11A6)
    a.emit(0xC5)                          # PUSH BC: original B,C
    a.emit(0xDD, 0x7E, 0x30)              # LD A,(IX+30H)
    a.emit(0xFE, 0x04)                    # CP 4
    a.emit(0x06, 0x02)                    # LD B,2
    a.jr(0x28, "base_ready")              # JR Z
    a.emit(0x06, 0x08)                    # LD B,8
    a.label("base_ready")
    a.emit(0x4B)                          # LD C,E
    a.call(helper)
    a.emit(0x04)                          # INC B
    a.emit(0x4A)                          # LD C,D
    a.call(helper)
    a.emit(0x04)                          # INC B: register 4 or 0A
    a.emit(0xD1)                          # POP DE: D=original B, E=original C
    a.emit(0x4B)                          # LD C,E
    a.call(helper)
    a.emit(0x78, 0xFE, 0x04)              # LD A,B / CP 4
    a.jr(0x20, "channel_1_bank")
    a.emit(0x3A, 0x17, 0x83)              # LD A,(8317H)
    a.emit(0xE6, 0x0C)                    # AND 0CH
    a.emit(0xB2)                          # OR D
    a.jr(0x18, "bank_ready")
    a.label("channel_1_bank")
    a.emit(0x3A, 0x17, 0x83)
    a.emit(0xE6, 0x03)
    a.emit(0x4A)                          # LD C,D
    a.emit(0xCB, 0x21, 0xCB, 0x21)        # SLA C twice
    a.emit(0xB1)                          # OR C
    a.label("bank_ready")
    a.emit(0x32, 0x17, 0x83)              # preserve original bank shadow
    a.emit(0x4F)                          # LD C,A
    a.emit(0x06, 0x10)                    # pseudo-register 10H = sample bank
    a.call(helper)
    a.emit(0xC9)
    result = a.resolve()
    if len(result) > 54:
        raise ValueError(f"11A6 patch is {len(result)} bytes, expected <=54")
    return result + bytes(54 - len(result))


def build_k007_frequency_patch(helper: int) -> bytes:
    """Replacement for 11DC-11ED, exactly 18 bytes."""
    a = Asm(0x11DC)
    a.emit(0xDD, 0x7E, 0x30)              # LD A,(IX+30H)
    a.emit(0xFE, 0x04)
    a.emit(0x06, 0x00)
    a.jr(0x28, "base_ready")
    a.emit(0x06, 0x06)
    a.label("base_ready")
    a.emit(0x4B)                          # LD C,E
    a.call(helper)
    a.emit(0x04)
    a.emit(0x4A)                          # LD C,D
    a.emit(0xC3); a.word(helper)           # JP helper; helper RET returns caller
    result = a.resolve()
    if len(result) != 18:
        raise ValueError(f"11DC patch is {len(result)} bytes, expected 18")
    return result


def build_engine(audio_rom: bytes) -> tuple[bytes, dict[str, int]]:
    image = bytearray(audio_rom)
    wrapper, labels = build_wrapper()

    # Callable KSP adaptation.
    patch_exact(image, 0x030A, bytes.fromhex("ed 56 31 00 86"), b"")
    patch_exact(image, 0x03CE, bytes.fromhex("fb"), bytes.fromhex("c9"))
    patch_exact(image, 0x0505, bytes.fromhex("c3 ce 03"), bytes.fromhex("c9"))

    # YM3812 -> MoonSound OPL2-compatible FM ports C4/C5. The small delay
    # before the data write mirrors the original routine's conservative pace.
    opl_patch = bytes.fromhex("78 d3 c4 3e 06 3d 20 fd 79 d3 c5 c9")
    patch_exact(image, 0x0F06, audio_rom[0x0F06:0x0F18], opl_patch)

    # K007232 shutdown calls reached during normal initialization.
    patch_exact(image, 0x01B2, bytes.fromhex("32 0d b0"), bytes([0xCD]) + struct.pack("<H", labels["k7_write_0d_a"]))
    patch_exact(image, 0x01B5, bytes.fromhex("32 0c b0"), bytes([0xCD]) + struct.pack("<H", labels["k7_write_0c_a"]))

    # Main K007232 helpers are replaced by jumps to the wrapper gateways.
    for address, end, label in (
        (0x11A6, 0x11DC, "k7_start"),
        (0x11DC, 0x11EE, "k7_frequency"),
        (0x11EE, 0x121E, "k7_volume"),
        (0x121E, 0x1234, "k7_trigger"),
        (0x1254, 0x1274, "k7_channel_stop"),
    ):
        replacement = bytes([0xC3]) + struct.pack("<H", labels[label])
        image[address:end] = replacement + bytes((end - address) - len(replacement))

    # ENGN is a contiguous image from 0000H through the wrapper. Work RAM
    # 8000H-87FFH is intentionally initialized to zero.
    engine_end = WRAPPER_BASE + len(wrapper)
    engine = bytearray(engine_end)
    engine[:0x8000] = image
    engine[WRAPPER_BASE:engine_end] = wrapper
    return bytes(engine), labels


def encode_descriptor(labels: dict[str, int]) -> bytes:
    output = bytearray(36)
    output[0:4] = b"KED1"
    struct.pack_into("<H", output, 4, 36)
    struct.pack_into("<H", output, 6, KSP_ENGINE_TYPE_NATIVE_Z80)
    struct.pack_into("<H", output, 8, 0x0000)
    struct.pack_into("<H", output, 10, labels["init"])
    struct.pack_into("<H", output, 12, labels["play"])
    struct.pack_into("<H", output, 14, labels["stop"])
    struct.pack_into("<H", output, 16, labels["capability"])
    struct.pack_into("<H", output, 18, SONG_WINDOW)
    struct.pack_into("<H", output, 20, 0x8000)
    struct.pack_into("<H", output, 22, 0x0800)
    struct.pack_into("<H", output, 24, 60)
    struct.pack_into("<H", output, 26, 1)
    struct.pack_into("<I", output, 28, 0)
    struct.pack_into("<I", output, 32, KSP_FLAGS)
    return bytes(output)


def build_kss_header(labels: dict[str, int]) -> bytes:
    h = bytearray(0x20)
    h[0:4] = b"KSSX"
    struct.pack_into("<H", h, 0x04, 0x0000)
    struct.pack_into("<H", h, 0x06, LOAD_SIZE)
    struct.pack_into("<H", h, 0x08, labels["init"])
    struct.pack_into("<H", h, 0x0A, labels["play"])
    h[0x0C] = 0
    h[0x0D] = 0
    h[0x0E] = 0x10
    h[0x0F] = 0x04                     # MSX RAM mode; SCC remains memory mapped
    struct.pack_into("<H", h, 0x18, 0)
    struct.pack_into("<H", h, 0x1A, 20)
    return bytes(h)


def write_ksp(path: Path, header: bytes, chunks: Iterable[Chunk]) -> dict:
    chunks = list(chunks)
    cursor = 0x20
    entries = []
    payload = bytearray(header)
    for chunk in chunks:
        offset = cursor
        payload.extend(chunk.data)
        cursor += len(chunk.data)
        entries.append(
            {
                "type": chunk.type,
                "id": chunk.id,
                "offset": offset,
                "packed_size": len(chunk.data),
                "unpacked_size": len(chunk.data),
                "crc32": zlib.crc32(chunk.data) & 0xFFFFFFFF,
                "compression": 0,
                "flags": 0,
                "aux": chunk.aux,
            }
        )

    directory = bytearray(16 + 32 * len(entries))
    directory[0:4] = b"KDIR"
    struct.pack_into("<H", directory, 4, 16)
    struct.pack_into("<H", directory, 6, 32)
    struct.pack_into("<I", directory, 8, len(entries))
    struct.pack_into("<I", directory, 12, 0)
    for i, entry in enumerate(entries):
        p = 16 + i * 32
        directory[p:p + 4] = entry["type"]
        struct.pack_into("<I", directory, p + 4, entry["id"])
        struct.pack_into("<I", directory, p + 8, entry["offset"])
        struct.pack_into("<I", directory, p + 12, entry["packed_size"])
        struct.pack_into("<I", directory, p + 16, entry["unpacked_size"])
        struct.pack_into("<I", directory, p + 20, entry["crc32"])
        struct.pack_into("<H", directory, p + 24, entry["compression"])
        struct.pack_into("<H", directory, p + 26, entry["flags"])
        struct.pack_into("<I", directory, p + 28, entry["aux"])

    directory_offset = len(payload)
    payload.extend(directory)
    trailer = bytearray(24)
    trailer[0:4] = b"KSP1"
    struct.pack_into("<H", trailer, 4, 24)
    struct.pack_into("<H", trailer, 6, 0)
    struct.pack_into("<I", trailer, 8, directory_offset)
    struct.pack_into("<I", trailer, 12, len(directory))
    struct.pack_into("<I", trailer, 16, zlib.crc32(directory) & 0xFFFFFFFF)
    struct.pack_into("<I", trailer, 20, 0)
    payload.extend(trailer)
    path.write_bytes(payload)
    return {
        "file_size": len(payload),
        "directory_offset": directory_offset,
        "directory_size": len(directory),
        "entries": [
            {**e, "type": e["type"].decode("ascii")}
            for e in entries
        ],
    }


def validate_ksp(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] != b"KSSX" or data[-24:-20] != b"KSP1":
        raise ValueError("missing KSSX header or KSP1 trailer")
    directory_offset, directory_size, directory_crc = struct.unpack_from("<III", data, len(data) - 16)
    directory = data[directory_offset:directory_offset + directory_size]
    if zlib.crc32(directory) & 0xFFFFFFFF != directory_crc:
        raise ValueError("directory CRC mismatch")
    if directory[:4] != b"KDIR":
        raise ValueError("directory magic mismatch")
    header_size, entry_size = struct.unpack_from("<HH", directory, 4)
    count = struct.unpack_from("<I", directory, 8)[0]
    if header_size != 16 or entry_size != 32 or len(directory) != 16 + count * 32:
        raise ValueError("directory dimensions are invalid")
    seen = set()
    decoded = []
    for i in range(count):
        p = 16 + i * 32
        kind = directory[p:p + 4].decode("ascii")
        ident, offset, packed, unpacked, crc = struct.unpack_from("<IIIII", directory, p + 4)
        compression, flags = struct.unpack_from("<HH", directory, p + 24)
        aux = struct.unpack_from("<I", directory, p + 28)[0]
        if (kind, ident) in seen:
            raise ValueError(f"duplicate chunk {kind}/{ident}")
        seen.add((kind, ident))
        chunk = data[offset:offset + packed]
        if compression != 0 or packed != unpacked:
            raise ValueError("this builder only emits uncompressed chunks")
        if zlib.crc32(chunk) & 0xFFFFFFFF != crc:
            raise ValueError(f"CRC mismatch for {kind}/{ident}")
        decoded.append({
            "type": kind,
            "id": ident,
            "offset": offset,
            "size": packed,
            "crc32": f"{crc:08x}",
            "flags": flags,
            "aux": aux,
        })
    required = {("ENGN", 0), ("EDES", 0)}
    if not required.issubset(seen) or not any(k == "SONG" for k, _ in seen):
        raise ValueError("missing required compact KSP chunks")
    return {
        "size": len(data),
        "sha1": sha1(data),
        "directory_offset": directory_offset,
        "entry_count": count,
        "chunks": decoded,
    }


def materialize_kss(header: bytes, engine: bytes, song: int) -> bytes:
    image = bytearray(LOAD_SIZE)
    image[:len(engine)] = engine
    image[SONG_WINDOW] = song & 0xFF
    return header + image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--audio-rom", type=Path, default=Path("768e01.e4"))
    parser.add_argument("--pcm-rom", type=Path, default=Path("768c07.e17"))
    parser.add_argument("--output", type=Path, default=Path("haunted_castle_full.ksp"))
    parser.add_argument("--report", type=Path, default=Path("haunted_castle_full_validation.json"))
    parser.add_argument("--test-kss", type=Path, default=Path("haunted_castle_full_materialized.kss"))
    args = parser.parse_args()

    audio = args.audio_rom.read_bytes()
    pcm = args.pcm_rom.read_bytes()
    if len(audio) != 0x8000 or sha1(audio) != AUDIO_SHA1:
        raise SystemExit("unsupported or corrupt 768e01.e4")
    if len(pcm) != 0x80000 or sha1(pcm) != PCM_SHA1:
        raise SystemExit("unsupported or corrupt 768c07.e17")

    engine, labels = build_engine(audio)
    descriptor = encode_descriptor(labels)
    header = build_kss_header(labels)

    hardware_map = {
        "format": "HC-HMAP-1",
        "engine": "Haunted Castle 768e01.e4",
        "clock": {"scheduler_hz": 60},
        "chips": {
            "K051649": {"target": "MSX SCC", "address": "0x9800"},
            "YM3812": {
                "target": "MoonSound FM in OPL2 mode",
                "ports": {
                    "bank0_address": "0xC4",
                    "bank0_data": "0xC5",
                    "bank1_address": "0xC6",
                    "bank1_data": "0xC7"
                },
                "initialization": "engine writes register 105H = 02H (NEW2=1, NEW=0)",
                "translation": "register-compatible; no player-side C4/C5 remap",
            },
            "K007232": {
                "target": "desktop emulation or MoonSound PCM translator",
                "virtual_ports": {"register": "0xD0", "data": "0xD1"},
                "registers": "0x00-0x0D",
                "sample_bank_pseudo_register": "0x10",
                "sample_rom_chunk": {"type": "K7RM", "id": 0},
                "sample_format": "7-bit unsigned centered PCM; bit 7 is the end marker",
            },
        },
        "engine_flags": f"0x{KSP_FLAGS:08X}",
    }
    metadata = {
        "format": "KSP metadata development record",
        "title": "Haunted Castle",
        "publisher": "Konami",
        "year": 1988,
        "source_audio_rom_sha1": AUDIO_SHA1,
        "source_pcm_rom_sha1": PCM_SHA1,
        "track_count": 21,
        "tracks": [
            {"id": i, "title": f"Sound command {0x50 + i:02X}H", "command": 0x50 + i}
            for i in range(21)
        ],
    }
    symbols = {
        "init": f"0x{labels['init']:04X}",
        "play": f"0x{labels['play']:04X}",
        "stop": f"0x{labels['stop']:04X}",
        "capability": f"0x{labels['capability']:04X}",
        "song_window": f"0x{SONG_WINDOW:04X}",
        "original_scheduler": "0x0476",
        "original_command_dispatcher": "0x003C",
        "original_ym3812_writer": "0x0F06 patched directly to C4/C5",
        "moonsound_init": f"0x{labels['moonsound_init']:04X} writes 105H=02H through C6/C7",
        "virtual_k007_ports": ["0xD0", "0xD1"],
    }

    chunks: list[Chunk] = [
        Chunk(b"ENGN", 0, engine),
        Chunk(b"EDES", 0, descriptor),
    ]
    chunks.extend(Chunk(b"SONG", i, bytes([i]), aux=0x50 + i) for i in range(21))
    chunks.extend(
        [
            Chunk(b"K7RM", 0, pcm),
            Chunk(b"HMAP", 0, json.dumps(hardware_map, indent=2).encode("utf-8")),
            Chunk(b"META", 0, json.dumps(metadata, indent=2).encode("utf-8")),
            Chunk(b"SYMB", 0, json.dumps(symbols, indent=2).encode("utf-8")),
        ]
    )

    layout = write_ksp(args.output, header, chunks)
    validation = validate_ksp(args.output)
    args.test_kss.write_bytes(materialize_kss(header, engine, 0xFF))

    report = {
        "output": str(args.output),
        "ksp": validation,
        "engine_size": len(engine),
        "descriptor": {
            "engine_type": KSP_ENGINE_TYPE_NATIVE_Z80,
            "load_address": "0x0000",
            "init_address": f"0x{labels['init']:04X}",
            "play_address": f"0x{labels['play']:04X}",
            "stop_address": f"0x{labels['stop']:04X}",
            "capability_address": f"0x{labels['capability']:04X}",
            "song_window_address": f"0x{SONG_WINDOW:04X}",
            "work_address": "0x8000",
            "work_size": "0x0800",
            "tick_rate": "60/1",
            "flags": f"0x{KSP_FLAGS:08X}",
        },
        "patches": {
            "ym3812": "engine writes directly to MoonSound C4/C5; INIT configures C6/C7 register 05H to 02H",
            "k007232": "runtime register accesses redirected to virtual ports D0/D1",
            "scheduler": "one 0476H channel pass per KSP PLAY call",
            "pcm_rom": "stored byte-for-byte as K7RM/0",
        },
        "source_hashes": {"768e01.e4": AUDIO_SHA1, "768c07.e17": PCM_SHA1},
        "materialized_test_kss": {"path": str(args.test_kss), "sha1": sha1(args.test_kss.read_bytes())},
        "writer_layout": layout,
        "compatibility_note": (
            "Current repository engine.c accepts only EDES engine_type 1. "
            "This KSP intentionally uses type 2 (native Z80); update the validator "
            "and materializer before opening it with the current desktop player."
        ),
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n")

    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes)")
    print(f"Wrote {args.test_kss} ({args.test_kss.stat().st_size} bytes)")
    print(f"Wrote {args.report}")
    print(f"KSP SHA-1 {validation['sha1']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
