#!/usr/bin/env python3
"""Build the MSX-DOS2 COM loader for unmodified raw KSS/KSSX files."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT / "msx")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    player_source = ROOT / "msx" / "BASICKSS.asm"
    loader_source = ROOT / "msx" / "KSSDOS2.asm"

    with tempfile.TemporaryDirectory(prefix="msx-dos2-kss-") as temp_name:
        temp = pathlib.Path(temp_name)
        player_raw = temp / "KSSDOS2_PLAYER.raw"
        loader_raw = temp / "KSSDOS2.raw"
        player_copy = temp / "BASICKSS.asm"
        loader_copy = temp / "KSSDOS2.asm"
        player_copy.write_bytes(player_source.read_bytes())
        loader_copy.write_bytes(loader_source.read_bytes())
        subprocess.run(
            [args.assembler, "-o", str(player_raw), player_copy.name],
            cwd=temp,
            check=True,
        )
        if len(player_raw.read_bytes()) > 0x8000:
            raise ValueError("embedded player does not fit below 8000H")
        subprocess.run(
            [args.assembler, "-o", str(loader_raw), loader_copy.name],
            cwd=temp,
            check=True,
        )
        payload = loader_raw.read_bytes()

    if len(payload) > 0xFF00:
        raise ValueError("COM payload is too large for the MSX-DOS TPA")
    output = args.output_dir / "KSSPLAY.COM"
    output.write_bytes(payload)
    print(f"wrote {output} ({len(payload)} bytes, load address 0100H)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
