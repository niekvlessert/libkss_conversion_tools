#!/usr/bin/env python3
"""Build an OpenMSX Disk-BASIC player for compressed Nemesis 3 KSSX."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


LOAD_ADDRESS = 0x4000
PLAYER_ADDRESS = 0xC000
ZX0_ADDRESS = 0xCF00
SCRATCH_ADDRESS = 0xE000
TRACK_COUNT = 24


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def relocated_zx0_decoder(address: int) -> bytes:
    decoder = bytearray.fromhex(
        "01ffffc5033e80cd3500edb087380dcd3500e3e519edb0e1e38730ebc10efe"
        "cd36000cc8414e23cb18cb19c5010100d43d000318dd0c8720037e2317d887"
        "cb11cb1018f2"
    )
    for offset, target in ((8, 0x35), (16, 0x35), (32, 0x36), (48, 0x3D)):
        struct.pack_into("<H", decoder, offset, address + target)
    return bytes(decoder)


def write_msx_binary(path: Path, payload: bytes, load: int, execute: int = 0) -> None:
    end = load + len(payload) - 1
    if end > 0xFFFF:
        raise ValueError(f"MSX binary ends above FFFFH: {end:04X}")
    path.write_bytes(struct.pack("<BHHH", 0xFE, load, end, execute) + payload)


def validate_kss(data: bytes) -> tuple[int, tuple[int, int, int]]:
    if len(data) < 0x20 or data[:4] != b"KSSX":
        raise ValueError("source is not a KSSX file")
    load_address, load_size = struct.unpack_from("<HH", data, 4)
    if (load_address, u16(data, 8), u16(data, 10)) != (0x0200, 0x0200, 0x5FE0):
        raise ValueError("source is not the expected compact Nemesis 3 KSSX")
    if data[0x0D] != 3 or data[0x0C] != 9:
        raise ValueError("source does not describe three 16K banks at offset 9")
    tail = 0x20 + load_size
    if tail + 10 > len(data) or data[tail : tail + 4] != b"N3ZX":
        raise ValueError("source has no N3ZX bank tail")
    lengths = struct.unpack_from("<HHH", data, tail + 4)
    if tail + 10 + sum(lengths) > len(data):
        raise ValueError("source has a truncated N3ZX bank tail")
    return load_size, lengths


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("vigamup/extracted/nemesis3_16kb_max_compressed.kss"),
    )
    parser.add_argument("--output-dir", type=Path, default=Path("msx"))
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument("--zx0pack", type=Path, help="accepted for build-script symmetry")
    args = parser.parse_args()

    source = args.source.resolve()
    data = source.read_bytes()
    load_size, lengths = validate_kss(data)
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # A large BLOAD at 4000H overwrites the active BASIC program, so it must be
    # the terminal operation.  Put the KSS image at 4000H, pad up to C000H,
    # append the player there, and execute the player directly from the same
    # BLOAD.  The player never returns to BASIC after this load.
    (output_dir / "NEM3RAW.KSS").write_bytes(data)

    template = Path(__file__).resolve().parents[1] / "msx" / "NEM3PLAY.asm"
    with tempfile.TemporaryDirectory(prefix="nem3-msx-player-") as temporary:
        work = Path(temporary)
        (work / "DZX0.BIN").write_bytes(relocated_zx0_decoder(ZX0_ADDRESS))
        source_text = template.read_text(encoding="ascii").replace(
            "@@DZX0@@", "DZX0.BIN"
        )
        (work / "NEM3PLAY.asm").write_text(source_text, encoding="ascii")
        raw = work / "NEM3PLAY.raw"
        subprocess.run(
            [args.assembler, "-o", str(raw), "NEM3PLAY.asm"],
            cwd=work,
            check=True,
        )
        payload = raw.read_bytes()

    if PLAYER_ADDRESS + len(payload) > SCRATCH_ADDRESS:
        raise ValueError(
            f"NEM3PLAY payload overlaps scratch at {SCRATCH_ADDRESS:04X}H: "
            f"ends at {PLAYER_ADDRESS + len(payload) - 1:04X}H"
        )
    player_image = bytes(data)
    if LOAD_ADDRESS + len(player_image) > PLAYER_ADDRESS:
        raise ValueError("KSS image overlaps the player load address")
    player_image += bytes(PLAYER_ADDRESS - (LOAD_ADDRESS + len(player_image)))
    player_image += payload
    wrapped_kss = struct.pack(
        "<BHHH", 0xFE, LOAD_ADDRESS, LOAD_ADDRESS + len(player_image) - 1, PLAYER_ADDRESS
    ) + player_image
    (output_dir / "NEM3COMP.KSS").write_bytes(wrapped_kss)
    write_msx_binary(output_dir / "NEM3PLAY.BIN", payload, PLAYER_ADDRESS, PLAYER_ADDRESS)
    (output_dir / "AUTOEXEC.BAS").write_bytes(
        b'10 CLS\r\n'
        b'20 PRINT "NEMESIS 3 COMPRESSED KSS PLAYER"\r\n'
        b'30 PRINT "Loading NEM3COMP.KSS..."\r\n'
        b'40 PRINT "Loading KSS and player..."\r\n'
        b'50 BLOAD "NEM3COMP.KSS",R\r\n'
    )
    (output_dir / "README_NEM3PLAY.md").write_text(
        "# Nemesis 3 compressed KSS MSX player\n\n"
        "In OpenMSX, mount this directory as drive A and run:\n\n"
        "```basic\nRUN \"AUTOEXEC.BAS\"\n```\n\n"
        "`NEM3COMP.KSS` is an MSX Disk-BASIC BLOAD image: it contains the exact "
        "compressed KSS payload at 4000H, followed by padding and the player at "
        "C000H. `NEM3RAW.KSS` is the unmodified KSS copy. The player expands the "
        "engine and RAM-mapper banks before INIT, then "
        "calls PLAY through H.TIMI. The machine must provide an MSX RAM mapper "
        "and the original KSS engine uses PSG/SCC hardware.\n",
        encoding="ascii",
    )

    print(f"wrote {output_dir / 'NEM3PLAY.BIN'} ({len(payload)} bytes)")
    print(f"wrote {output_dir / 'NEM3COMP.KSS'} ({len(wrapped_kss)} bytes, KSS + player BLOAD image)")
    print(f"wrote {output_dir / 'NEM3RAW.KSS'} ({len(data)} bytes, exact KSS copy)")
    print(f"  load image: {load_size} bytes; bank streams: {lengths}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
