#!/usr/bin/env python3
"""Build the BASIC-launched raw KSS/KSSX MSX player."""

import argparse
import pathlib
import struct
import subprocess
import tempfile


def write_msx_binary(path: pathlib.Path, payload: bytes) -> None:
    load = 0xC000
    end = load + len(payload) - 1
    if end > 0xFFFF:
        raise ValueError(f"BASICKSS.BIN ends above FFFFH: {end:04X}")
    path.write_bytes(struct.pack("<BHHH", 0xFE, load, end, load) + payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("msx"))
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[1]
    source = root / "msx" / "BASICKSS.asm"
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="basickss-") as temporary:
        raw = pathlib.Path(temporary) / "BASICKSS.raw"
        subprocess.run([args.assembler, "-o", str(raw), str(source)], check=True)
        payload = raw.read_bytes()

    write_msx_binary(output_dir / "BASICKSS.BIN", payload)
    print(f"wrote {output_dir / 'BASICKSS.BIN'} ({len(payload)} bytes, C000-{0xC000 + len(payload) - 1:04X}H)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
