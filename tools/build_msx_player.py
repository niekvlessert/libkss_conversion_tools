#!/usr/bin/env python3
"""Build the BASIC-launched, ZX0-packed MSX KSP test player."""

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


def relocated_zx0_decoder(address: int) -> bytes:
    decoder = bytearray.fromhex(
        "01ffffc5033e80cd3500edb087380dcd3500e3e519edb0e1e38730ebc10efe"
        "cd36000cc8414e23cb18cb19c5010100d43d000318dd0c8720037e2317d887"
        "cb11cb1018f2"
    )
    for offset, target in ((8, 0x35), (16, 0x35), (32, 0x36), (48, 0x3D)):
        struct.pack_into("<H", decoder, offset, address + target)
    return bytes(decoder)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ksp", type=pathlib.Path,
                        default=pathlib.Path(
                            "dutch_moonsound_veterans/ksp/DMV1/ALMOSEND_compressed.ksp"
                        ))
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("msx"))
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument(
        "--kspmaterialize",
        type=pathlib.Path,
        help="kspmaterialize executable (default: build-moonsound/kspmaterialize or build/kspmaterialize)",
    )
    parser.add_argument(
        "--zx0pack",
        type=pathlib.Path,
        help="zx0pack executable (default: build-moonsound/zx0pack or build/zx0pack)",
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

    entry_stem = ksp_path.stem
    if entry_stem.lower().endswith("_compressed"):
        entry_stem = entry_stem[:-len("_compressed")]
    entry_stem = entry_stem.upper()[:8]
    entry_name = entry_stem + ".KSP"
    output_ksp = output_dir / entry_name
    shutil.copyfile(ksp_path, output_ksp)
    source_text = source_template.read_text(encoding="ascii")
    source_text = source_text.replace("@@ENTRY@@", entry_name)
    source_text = source_text.replace("@@ENGINE_ZX0@@", "ENGINE.ZX0")
    source_text = source_text.replace("@@SONG_ZX0@@", "SONG.ZX0")
    source_text = source_text.replace("@@DZX0@@", "DZX0.BIN")
    (output_dir / "KSPPLAY.BAS").write_bytes(
        b'10 CLS\r\n'
        b'20 PRINT "KSP ZX0 MOONSOUND PLAYBACK"\r\n'
        b'30 PRINT "Loading compact engine and song..."\r\n'
        b'40 BLOAD "KSPPLAY.BIN",R\r\n'
    )
    (output_dir / "README.md").write_text(
        "# MSX KSP player disk\n\n"
        "Mount this directory as a disk in openMSX with Disk BASIC. Then run:\n\n"
        "```basic\nRUN \"KSPPLAY.BAS\"\n```\n\n"
        f"The machine-code bootstrap displays progress, starts the embedded `{entry_name}`"
        " payload, initializes track 0, and drives playback at 60 Hz."
        " The materialized engine and song are independently ZX0-packed inside"
        " `KSPPLAY.BIN`; the BIN remains below C000H so Disk BASIC can load it safely."
        f" BASIC/BIN version; `{entry_name}` is kept on disk as the compact source archive."
        " A MoonSound/OPL4 cartridge with its YRW801 ROM is required.\n",
        encoding="ascii",
    )

    with tempfile.TemporaryDirectory(prefix="ksp-msx-") as temporary:
        temporary_dir = pathlib.Path(temporary)
        if len(prefix) <= 0x4000:
            raise ValueError("materialized KSP has no song window above 8000H")
        (temporary_dir / "ENGINE.RAW").write_bytes(prefix[:0x4000])
        (temporary_dir / "SONG.RAW").write_bytes(prefix[0x4000:])
        pack_candidates = [args.zx0pack] if args.zx0pack else [
            pathlib.Path("build-moonsound/zx0pack"),
            pathlib.Path("build/zx0pack"),
        ]
        packer = next((path.resolve() for path in pack_candidates if path and path.is_file()), None)
        if packer is None:
            raise ValueError("zx0pack was not found; build the zx0pack CMake target first")
        subprocess.run([str(packer), "ENGINE.RAW", "ENGINE.ZX0"],
                       cwd=temporary_dir, check=True)
        subprocess.run([str(packer), "SONG.RAW", "SONG.ZX0"],
                       cwd=temporary_dir, check=True)
        (temporary_dir / "DZX0.BIN").write_bytes(relocated_zx0_decoder(0xBF00))
        assembled = temporary_dir / "KSPPLAY.raw"
        source = temporary_dir / "KSPPLAY.asm"
        source.write_text(source_text, encoding="ascii")
        subprocess.run([args.assembler, "-o", str(assembled), source.name],
                       cwd=temporary_dir, check=True)
        payload = assembled.read_bytes()
    write_msx_binary(output_dir / "KSPPLAY.BIN", payload, 0x8000, 0x8000)

    print(f"wrote {output_dir / 'KSPPLAY.BIN'} ({len(payload)} bytes of Z80 payload)")
    print(f"wrote {output_ksp} ({output_ksp.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
