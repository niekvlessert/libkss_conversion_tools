#!/usr/bin/env python3
"""Build a real-MSX loader for the maximally compressed F-1 Spirit KSS."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


DATA_ADDRESS = 0x4000
PLAYER_ADDRESS = 0xC000


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def write_msx_binary(path: Path, payload: bytes, load: int, execute: int = 0) -> None:
    end = load + len(payload) - 1
    path.write_bytes(struct.pack("<BHHH", 0xFE, load, end, execute) + payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("vigamup/extracted/f1spirit_16kb_max_compressed.kss"),
    )
    parser.add_argument("--output-dir", type=Path, default=Path("msx"))
    parser.add_argument("--assembler", default="z80asm")
    args = parser.parse_args()

    data = args.source.read_bytes()
    if data[:4] != b"KSSX":
        parser.error("source is not KSSX")
    load_address = word(data, 4)
    load_size = word(data, 6)
    if (load_address, word(data, 8), word(data, 10)) != (0x0200, 0x0200, 0x5F80):
        parser.error("source is not the expected compressed F-1 Spirit image")
    if data[0x0C] != 14 or data[0x0D] != 0:
        parser.error("source does not describe F-1 Spirit bank 14")
    image = data[0x20 : 0x20 + load_size]
    if len(image) != load_size:
        parser.error("source load image is truncated")

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    template = Path(__file__).resolve().parents[1] / "msx" / "F1PLAY.asm"
    source_text = template.read_text(encoding="ascii").replace(
        "@@DATA_LENGTH@@", f"0x{load_size:04X}"
    )
    with tempfile.TemporaryDirectory(prefix="f1-msx-player-") as temporary:
        work = Path(temporary)
        assembly = work / "F1PLAY.asm"
        raw = work / "F1PLAY.raw"
        assembly.write_text(source_text, encoding="ascii")
        subprocess.run(
            [args.assembler, "-o", str(raw), assembly.name], cwd=work, check=True
        )
        player = raw.read_bytes()

    write_msx_binary(output_dir / "F1DATA.BIN", image, DATA_ADDRESS)
    write_msx_binary(output_dir / "F1PLAY.BIN", player, PLAYER_ADDRESS, PLAYER_ADDRESS)
    (output_dir / "AUTOEXEC.BAS").write_bytes(
        b'10 CLS\r\n'
        b'20 PRINT "F-1 SPIRIT MAX-COMPRESSED PLAYER"\r\n'
        b'30 PRINT "LOADING DATA..."\r\n'
        b'40 BLOAD "F1DATA.BIN"\r\n'
        b'50 PRINT "STARTING PLAYER..."\r\n'
        b'60 BLOAD "F1PLAY.BIN",R\r\n'
    )
    print(f"wrote {output_dir / 'F1DATA.BIN'} ({load_size} data bytes)")
    print(f"wrote {output_dir / 'F1PLAY.BIN'} ({len(player)} player bytes)")
    print(f"wrote {output_dir / 'AUTOEXEC.BAS'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
