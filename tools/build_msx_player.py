#!/usr/bin/env python3
"""Build the BASIC-launched MSX KSP player disk directory.

The current diagnostic MSX bootstrap embeds a materialized KSS image in
KSPPLAY.BIN. Compact KSP files are materialized with kspmaterialize first;
the original compact KSP remains beside it as the selectable source archive.
"""

import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile


def read_kss_prefix(path: pathlib.Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 0x20 or data[:4] != b"KSSX":
        raise ValueError(f"{path} does not begin with KSSX")
    load_address, load_size = struct.unpack_from("<HH", data, 4)
    if load_address != 0x4000:
        raise ValueError(f"{path} has unsupported load address {load_address:04X}")
    prefix_size = 0x20 + load_size
    if prefix_size > len(data):
        raise ValueError(f"{path} has a truncated KSS prefix")
    return data[0x20:prefix_size]


def write_msx_binary(path: pathlib.Path, payload: bytes, load: int, execute: int) -> None:
    end = load + len(payload) - 1
    if end > 0xFFFF:
        raise ValueError(f"MSX binary ends above FFFFH: {end:04X}")
    path.write_bytes(struct.pack("<BHHH", 0xFE, load, end, execute) + payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ksp", type=pathlib.Path,
                        default=pathlib.Path("dutch_moonsound_veterans/ksp/DMV1/INTRO1.ksp"))
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("msx"))
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument(
        "--kspmaterialize",
        type=pathlib.Path,
        help="kspmaterialize executable (default: build-moonsound/kspmaterialize or build/kspmaterialize)",
    )
    args = parser.parse_args()

    source_template = pathlib.Path(__file__).resolve().parents[1] / "msx" / "KSPPLAY.asm"
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    ksp_path = args.ksp.resolve()
    try:
        prefix = read_kss_prefix(ksp_path)
    except ValueError as direct_error:
        candidates = [args.kspmaterialize] if args.kspmaterialize else [
            pathlib.Path("build-moonsound/kspmaterialize"),
            pathlib.Path("build/kspmaterialize"),
        ]
        materializer = next((path.resolve() for path in candidates if path and path.is_file()), None)
        if materializer is None:
            raise ValueError(
                f"{ksp_path} is a compact KSP; kspmaterialize was not found ({direct_error})"
            )
        with tempfile.TemporaryDirectory(prefix="ksp-materialize-") as temporary:
            materialized = pathlib.Path(temporary) / "materialized.kss"
            subprocess.run([str(materializer), str(ksp_path), str(materialized)], check=True)
            prefix = read_kss_prefix(materialized)

    entry_name = ksp_path.stem.upper() + ".KSP"
    pfx_name = ksp_path.stem.upper() + ".PFX"
    output_ksp = output_dir / entry_name
    output_pfx = output_dir / pfx_name
    shutil.copyfile(ksp_path, output_ksp)
    output_pfx.write_bytes(prefix)
    source_text = source_template.read_text(encoding="ascii")
    source_text = source_text.replace("@@ENTRY@@", entry_name)
    source_text = source_text.replace("@@PFX@@", pfx_name)
    (output_dir / "KSPPLAY.BAS").write_bytes(
        b'10 CLS\r\n'
        b'20 PRINT "KSP STAGE 7: VDP-CLOCKED PLAYBACK"\r\n'
        b'30 PRINT "INIT returns, then engine runs at 60 Hz."\r\n'
        b'40 BLOAD "KSPPLAY.BIN",R\r\n'
    )
    (output_dir / "README.md").write_text(
        "# MSX KSP player disk\n\n"
        "Mount this directory as a disk in openMSX with Disk BASIC. Then run:\n\n"
        "```basic\nRUN \"KSPPLAY.BAS\"\n```\n\n"
        f"The machine-code bootstrap displays progress, starts the embedded `{entry_name}`"
        " payload, initializes track 0, and waits for the MoonSound interrupt hook."
        " The materialized KSS image is embedded in `KSPPLAY.BIN` for this first"
        f" BASIC/BIN version; `{entry_name}` is kept on disk as the compact source archive."
        " A MoonSound/OPL4 cartridge with its YRW801 ROM is required.\n",
        encoding="ascii",
    )

    with tempfile.TemporaryDirectory(prefix="ksp-msx-") as temporary:
        temporary_dir = pathlib.Path(temporary)
        shutil.copyfile(output_pfx, temporary_dir / pfx_name)
        assembled = temporary_dir / "KSPPLAY.raw"
        source = temporary_dir / "KSPPLAY.asm"
        source.write_text(source_text, encoding="ascii")
        subprocess.run([args.assembler, "-o", str(assembled), source.name],
                       cwd=temporary_dir, check=True)
        payload = assembled.read_bytes()
    write_msx_binary(output_dir / "KSPPLAY.BIN", payload, 0x9000, 0x9000)

    print(f"wrote {output_dir / 'KSPPLAY.BIN'} ({len(payload)} bytes of Z80 payload)")
    print(f"wrote {output_ksp} ({output_ksp.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
