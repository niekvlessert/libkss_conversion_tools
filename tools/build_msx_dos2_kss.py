#!/usr/bin/env python3
"""Build the MSX-DOS2 COM loader for unmodified raw KSS/KSSX files."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]


TARGET_VALUES = {
    "PLAYER_BASE": "0x52E4",
    "PLAYER_RUNTIME_ENTRY": "0x52F7",
    "RUNTIME_CONFIG": "0x5C47",
    "RETURN_STUB": "0x59C1",
    "PLAY_WRAPPER": "0x59CA",
    "PLAY_TARGET": "0x5981",
    "KSS_TABLE_SAVED": "0x6000",
    "SCC_SHADOW": "0x6020",
    "SCC_PLUS_SHADOW": "0x6120",
    "KSS_STORAGE_SAVED": "0x6220",
    "RAM_SLOT_SAVED": "0x6221",
    "PAGE2_RESTORE_SAVED": "0x6222",
    "PAGE1_RESTORE_SAVED": "0x6223",
    "BANK0_HANDLER": "0x5A27",
    "BANK1_HANDLER": "0x5AA7",
    "CUSTOM_ENASLT": "0x5AF7",
    "PUT_P0_DISPATCH": "0x5B1B",
    "PUT_P1_DISPATCH": "0x5B1F",
    "PUT_P2_DISPATCH": "0x5B23",
    "RST28_SAVED": "0x6224",
    "RST28_INSTALLED": "0x6228",
    "SLOT_A8_SAVED": "0x6229",
    "SLOT_FFFF_SAVED": "0x622A",
    "HTIMI_SAVED": "0x622C",
    "HTIMI_INSTALLED": "0x6231",
    "RUNTIME_MARKER": "0x6232",
}


def target_layout(source: str) -> str:
    """Relayout the shared player source into Quarth's page-1 tail."""
    lines = []
    for line in source.splitlines():
        if line.strip().startswith("org     0xC000"):
            line = line.replace("0xC000", "0x52E4")
        stripped = line.strip()
        name = stripped.split(":", 1)[0] if ":" in stripped else ""
        if name in TARGET_VALUES and "equ" in line:
            prefix = line[: line.index("equ") + len("equ")]
            line = f"{prefix} {TARGET_VALUES[name]}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def parse_labels(output: str) -> dict[str, int]:
    labels: dict[str, int] = {}
    for line in output.splitlines():
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*):\s+equ\s+\$([0-9A-Fa-f]+)\s*$", line)
        if match:
            labels[match.group(1)] = int(match.group(2), 16)
    return labels


def check_runtime_layout(labels: dict[str, int]) -> None:
    """Validate the source-to-fixed-copy layout used by the resident player."""
    required = {
        "runtime_ready_start",
        "player_end",
        "return_stub",
        "return_stub_end",
        "play_wrapper",
        "play_wrapper_end",
        "bank0_handler",
        "bank0_handler_end",
        "bank1_handler",
        "bank1_handler_end",
        "custom_enaslt",
        "custom_enaslt_end",
        "put_p0_dispatch",
        "put_p0_dispatch_end",
        "put_p1_dispatch",
        "put_p1_dispatch_end",
        "put_p2_dispatch",
        "put_p2_dispatch_end",
    }
    missing = sorted(required - labels.keys())
    if missing:
        raise ValueError(f"runtime label output is missing: {', '.join(missing)}")

    runtime_entry = int(TARGET_VALUES["PLAYER_RUNTIME_ENTRY"], 16)
    if labels["runtime_ready_start"] != runtime_entry:
        raise ValueError(
            f"runtime entry moved to {labels['runtime_ready_start']:04X}; "
            f"expected {runtime_entry:04X}"
        )

    resident_limit = int(TARGET_VALUES["RUNTIME_MARKER"], 16) + 2
    if labels["player_end"] > resident_limit:
        raise ValueError(
            f"runtime ends at {labels['player_end']:04X}, beyond reserved tail {resident_limit:04X}"
        )

    placements = [
        ("RETURN_STUB", "return_stub", "return_stub_end"),
        ("PLAY_WRAPPER", "play_wrapper", "play_wrapper_end"),
        ("BANK0_HANDLER", "bank0_handler", "bank0_handler_end"),
        ("BANK1_HANDLER", "bank1_handler", "bank1_handler_end"),
        ("CUSTOM_ENASLT", "custom_enaslt", "custom_enaslt_end"),
        ("PUT_P0_DISPATCH", "put_p0_dispatch", "put_p0_dispatch_end"),
        ("PUT_P1_DISPATCH", "put_p1_dispatch", "put_p1_dispatch_end"),
        ("PUT_P2_DISPATCH", "put_p2_dispatch", "put_p2_dispatch_end"),
    ]
    ranges: list[tuple[int, int, str]] = []
    for destination_name, start_name, end_name in placements:
        start = labels[start_name]
        end = labels[end_name]
        if end <= start:
            raise ValueError(f"empty runtime block: {start_name}")
        destination = int(TARGET_VALUES[destination_name], 16)
        size = end - start
        if start < destination < end:
            raise ValueError(
                f"copy destination overlaps source in the unsafe direction: {destination_name}"
            )
        if destination < int(TARGET_VALUES["PLAYER_BASE"], 16):
            raise ValueError(f"runtime destination before player base: {destination_name}")
        if destination + size > resident_limit:
            raise ValueError(f"runtime block exceeds resident tail: {destination_name}")
        ranges.append((destination, destination + size, destination_name))

    ranges.sort()
    for (_, previous_end, previous_name), (current_start, _, current_name) in zip(ranges, ranges[1:]):
        if current_start < previous_end:
            raise ValueError(f"resident blocks overlap: {previous_name} and {current_name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT / "msx")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    player_source = ROOT / "msx" / "BASICKSS.asm"
    loader_source = ROOT / "msx" / "KSSDOS2.asm"

    temp = ROOT / "tmp" / "msx-dos2-kss-build"
    temp.mkdir(parents=True, exist_ok=True)
    player_raw = temp / "KSSDOS2_PLAYER.raw"
    runtime_raw = temp / "KSSDOS2_RUNTIME.raw"
    loader_raw = temp / "KSSDOS2.raw"
    player_copy = temp / "BASICKSS.asm"
    loader_copy = temp / "KSSDOS2.asm"
    player_source_text = player_source.read_text()
    player_copy.write_text(player_source_text)
    loader_copy.write_bytes(loader_source.read_bytes())
    layout = ROOT / "msx" / "PLAYER_LAYOUT.inc"
    (temp / "PLAYER_LAYOUT.inc").write_text(layout.read_text())
    subprocess.run(
        [args.assembler, "-o", str(player_raw), player_copy.name],
        cwd=temp,
        check=True,
    )
    if len(player_raw.read_bytes()) > 0x4000:
        raise ValueError("bootstrap player is unexpectedly large")
    (temp / "PLAYER_LAYOUT.inc").write_text(target_layout(layout.read_text()))
    player_copy.write_text(
        player_source_text.replace(
            "copy_field:\n",
            "runtime_ready_start:\n        jp      runtime_ready_impl\n\ncopy_field:\n",
            1,
        )
    )
    runtime_result = subprocess.run(
        [args.assembler, "-L", "-o", str(runtime_raw), player_copy.name],
        cwd=temp,
        check=True,
        capture_output=True,
        text=True,
    )
    check_runtime_layout(parse_labels(runtime_result.stdout + runtime_result.stderr))
    if 0x52E4 + len(runtime_raw.read_bytes()) > 0x8000:
        raise ValueError("page-1 Quarth runtime does not fit below 8000H")
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
