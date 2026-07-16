#!/usr/bin/env python3
"""Build KSSPLAYS.BIN, the five-track DMV music-ROM selector."""

import argparse
import pathlib
import struct
import subprocess
import tempfile


def relocated_zx0_decoder(address: int) -> bytes:
    decoder = bytearray.fromhex(
        "01ffffc5033e80cd3500edb087380dcd3500e3e519edb0e1e38730ebc10efe"
        "cd36000cc8414e23cb18cb19c5010100d43d000318dd0c8720037e2317d887"
        "cb11cb1018f2"
    )
    for offset, target in ((8, 0x35), (16, 0x35), (32, 0x36), (48, 0x3D)):
        struct.pack_into("<H", decoder, offset, address + target)
    return bytes(decoder)


def write_msx_binary(path: pathlib.Path, payload: bytes) -> None:
    load = 0xC000
    end = load + len(payload) - 1
    if end >= 0xF000:
        raise ValueError(f"ROM selector is too large: ends at {end:04X}H")
    path.write_bytes(struct.pack("<BHHH", 0xFE, load, end, load) + payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assembler", default="z80asm")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=pathlib.Path("msx"))
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[1]
    template = root / "msx" / "KSSPLAYS.asm"
    args.output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="ksp-rom-selector-") as temporary:
        work = pathlib.Path(temporary)
        (work / "DZX0.BIN").write_bytes(relocated_zx0_decoder(0xCF00))
        source = template.read_text(encoding="ascii").replace(
            "@@DZX0@@", "DZX0.BIN"
        )
        (work / "KSSPLAYS.asm").write_text(source, encoding="ascii")
        raw = work / "KSSPLAYS.raw"
        subprocess.run(
            [args.assembler, "-o", str(raw), "KSSPLAYS.asm"],
            cwd=work,
            check=True,
        )
        payload = raw.read_bytes()

    output = args.output_dir / "KSSPLAYS.BIN"
    write_msx_binary(output, payload)
    (args.output_dir / "KSSPLAYS.BAS").write_bytes(
        b'10 CLS\r\n'
        b'20 PRINT "DMV ROM MOONSOUND PLAYER"\r\n'
        b'30 PRINT "1 ALMOSEND"\r\n'
        b'40 PRINT "2 ANGEL_01"\r\n'
        b'50 PRINT "3 ANGEL_06"\r\n'
        b'60 PRINT "4 ANGEL_09"\r\n'
        b'70 PRINT "5 CLIMAX"\r\n'
        b'80 PRINT "SELECT 1-5:"\r\n'
        b'90 A$=INPUT$(1)\r\n'
        b'100 IF A$<"1" OR A$>"5" THEN 90\r\n'
        b'110 POKE &HBFFF,ASC(A$)-49\r\n'
        b'120 PRINT "LOADING ENGINE AND TRACK..."\r\n'
        b'130 BLOAD "KSSPLAYS.BIN",R\r\n'
    )
    print(
        f"wrote {output} ({len(payload)} bytes, "
        f"C000-{0xC000 + len(payload) - 1:04X}H)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
