#!/usr/bin/env python3
"""Build the MSX-DOS2 KSP player for compact MoonSound and Konami KCP."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]


def parse_labels(output: str) -> dict[str, int]:
    labels: dict[str, int] = {}
    for line in output.splitlines():
        match = re.match(
            r"^\s*([A-Za-z_][A-Za-z0-9_]*):\s+equ\s+\$([0-9A-Fa-f]+)\s*$",
            line,
        )
        if match:
            labels[match.group(1)] = int(match.group(2), 16)
    return labels


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=ROOT / "msx")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    temp = ROOT / "tmp" / "msx-dos2-ksp-build"
    temp.mkdir(parents=True, exist_ok=True)

    player_copy = temp / "KSPPLAYER.asm"
    loader_copy = temp / "KSPDOS2.asm"
    layout_copy = temp / "PLAYER_LAYOUT.inc"
    player_raw = temp / "KSPDOS2_PLAYER.raw"
    loader_raw = temp / "KSPDOS2.raw"

    player_copy.write_bytes((ROOT / "msx" / "KSPPLAYER.asm").read_bytes())
    loader_copy.write_bytes((ROOT / "msx" / "KSPDOS2.asm").read_bytes())
    layout_copy.write_bytes((ROOT / "msx" / "PLAYER_LAYOUT.inc").read_bytes())

    player_result = subprocess.run(
        [args.assembler, "-L", "-o", str(player_raw), player_copy.name],
        cwd=temp,
        check=True,
        capture_output=True,
        text=True,
    )
    labels = parse_labels(player_result.stdout + player_result.stderr)
    required = {"KCPX_SCRATCH", "KCPX_SCRATCH_SIZE", "player_end",
                "PUT_P0_DISPATCH"}
    missing = sorted(required - labels.keys())
    if missing:
        raise ValueError(f"player label output is missing: {', '.join(missing)}")
    scratch_start = labels["KCPX_SCRATCH"]
    scratch_end = scratch_start + labels["KCPX_SCRATCH_SIZE"]
    if labels["player_end"] > scratch_start:
        raise ValueError(
            f"bootstrap ends at {labels['player_end']:04X}, "
            f"overlapping scratch at {scratch_start:04X}")
    if scratch_end > labels["PUT_P0_DISPATCH"]:
        raise ValueError(
            f"scratch ends at {scratch_end:04X}, overlapping fixed "
            f"dispatches at {labels['PUT_P0_DISPATCH']:04X}")
    if len(player_raw.read_bytes()) > 0x4000:
        raise ValueError("bootstrap player is unexpectedly large")

    subprocess.run(
        [args.assembler, "-o", str(loader_raw), loader_copy.name],
        cwd=temp,
        check=True,
    )
    payload = loader_raw.read_bytes()
    if len(payload) > 0xFF00:
        raise ValueError("COM payload is too large for the MSX-DOS TPA")
    output = args.output_dir / "KSPPLAY.COM"
    output.write_bytes(payload)
    print(f"wrote {output} ({len(payload)} bytes, load address 0100H)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
