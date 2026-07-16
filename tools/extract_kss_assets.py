#!/usr/bin/env python3
"""Extract engines and track resources from legacy KSS files.

Most KSS files put the replayer in the initial load image and the music in
mapper banks.  A few Konami drivers, including Quarth, put both in the initial
load image.  Those drivers need a small, source-specific memory layout so the
semantic engine/resource split is not confused with the KSS file split.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


HEADER_SIZE = 0x10


# Ranges use absolute Z80 addresses and an exclusive end address.  The engine
# is stored in the output file as the concatenation of these ranges, while the
# metadata retains the addresses needed to map the fragments back into memory.
INTERNAL_LAYOUTS = {
    "quarth": {
        "engine_segments": ((0x4000, 0x52A4), (0xB800, 0xB824)),
        "track_segments": ((0x52A4, 0xB800),),
        "track_table_address": 0x52A2,
        "track_stream_address": 0x52E8,
        "expected_load_address": 0x4000,
        "expected_load_size": 0x7824,
        "expected_init_address": 0xB817,
        "expected_play_address": 0xB820,
    },
}


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def extract_image_segments(
    image: bytes,
    load_address: int,
    segments: tuple[tuple[int, int], ...],
) -> bytes:
    """Return absolute-addressed image ranges concatenated in segment order."""
    image_end = load_address + len(image)
    result = bytearray()
    for start, end in segments:
        if start < load_address or end < start or end > image_end:
            raise ValueError(
                "internal segment outside KSS load image: "
                f"0x{start:04X}:0x{end:04X} not within "
                f"0x{load_address:04X}:0x{image_end:04X}"
            )
        start_offset = start - load_address
        end_offset = end - load_address
        result.extend(image[start_offset:end_offset])
    return bytes(result)


def format_segments(segments: tuple[tuple[int, int], ...]) -> str:
    return ",".join(f"0x{start:04X}:0x{end:04X}" for start, end in segments)


def parse_kss(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE or data[:4] not in (b"KSCC", b"KSSX"):
        raise ValueError(f"{path} is not a KSCC/KSSX file")

    load_address = u16(data, 4)
    load_size = u16(data, 6)
    bank_offset = data[0x0C]
    bank_count = data[0x0D] & 0x7F
    bank_mode = 8 if data[0x0D] & 0x80 else 16
    bank_size = 0x2000 if bank_mode == 8 else 0x4000
    bank_data_size = bank_count * bank_size
    engine_end = HEADER_SIZE + load_size
    tracks_end = engine_end + bank_data_size

    if load_address + load_size > 0x10000:
        raise ValueError(f"{path} has a load image outside Z80 memory")
    if tracks_end > len(data):
        raise ValueError(f"{path} is truncated: needs {tracks_end} bytes")
    if data[tracks_end:] and len(data[tracks_end:]) != data[0x0E]:
        raise ValueError(
            f"{path} has {len(data[tracks_end:])} trailing bytes but header says "
            f"{data[0x0E]}"
        )

    main_image = data[HEADER_SIZE:engine_end]
    internal_layout = INTERNAL_LAYOUTS.get(path.stem.casefold())
    if internal_layout:
        expected_fields = (
            ("load address", load_address, internal_layout["expected_load_address"]),
            ("load size", load_size, internal_layout["expected_load_size"]),
            ("init address", u16(data, 8), internal_layout["expected_init_address"]),
            ("play address", u16(data, 0x0A), internal_layout["expected_play_address"]),
        )
        for field, actual, expected in expected_fields:
            if actual != expected:
                raise ValueError(
                    f"{path} does not match the {path.stem} internal layout: "
                    f"{field} is 0x{actual:04X}, expected 0x{expected:04X}"
                )
        if bank_count:
            raise ValueError(
                f"{path} does not match the {path.stem} internal layout: "
                "unexpected bank data"
            )
        engine_segments = internal_layout["engine_segments"]
        track_segments = internal_layout["track_segments"]
        engine = extract_image_segments(main_image, load_address, engine_segments)
        tracks = extract_image_segments(main_image, load_address, track_segments)
        layout_name = "embedded"
    else:
        engine_segments = ((load_address, load_address + load_size),)
        track_segments = ()
        engine = main_image
        tracks = data[engine_end:]
        layout_name = "kss-banked"

    return {
        "source": path.name,
        "format": data[:4].decode("ascii"),
        "load_address": load_address,
        "load_size": load_size,
        "init_address": u16(data, 8),
        "play_address": u16(data, 0x0A),
        "bank_offset": bank_offset,
        "bank_count": bank_count,
        "bank_mode": bank_mode,
        "bank_size": bank_size,
        "extra_size": data[0x0E],
        "device_flags": data[0x0F],
        "track_min": u16(data, 0x18) if data[:4] == b"KSSX" else 0,
        "track_max": u16(data, 0x1A) if data[:4] == b"KSSX" else 255,
        "layout_name": layout_name,
        "engine_segments": engine_segments,
        "track_segments": track_segments,
        "engine": engine,
        "tracks": tracks,
        "bank_data_size": bank_data_size,
        "file_size": len(data),
    }


def read_sidecar(path: Path, suffix: str) -> str:
    sidecar = path.with_suffix(suffix)
    if not sidecar.is_file():
        return ""
    return sidecar.read_text(encoding="utf-8", errors="replace").strip()


def write_one(source: Path, output_dir: Path) -> None:
    info = parse_kss(source)
    stem = source.stem
    engine_path = output_dir / f"{stem}.engine"
    tracks_path = output_dir / f"{stem}.tracks"
    text_path = output_dir / f"{stem}.txt"

    engine_path.write_bytes(info["engine"])
    tracks_path.write_bytes(info["tracks"])

    lines = [
        f"source={info['source']}",
        f"format={info['format']}",
        f"file_size={info['file_size']}",
        f"load_address=0x{info['load_address']:04X}",
        f"load_size=0x{info['load_size']:04X}",
        f"init_address=0x{info['init_address']:04X}",
        f"play_address=0x{info['play_address']:04X}",
        f"bank_offset={info['bank_offset']}",
        f"bank_count={info['bank_count']}",
        f"bank_mode={info['bank_mode']}K",
        f"bank_size=0x{info['bank_size']:04X}",
        f"bank_data_size=0x{info['bank_data_size']:04X}",
        f"extra_size={info['extra_size']}",
        f"device_flags=0x{info['device_flags']:02X}",
        f"track_min={info['track_min']}",
        f"track_max={info['track_max']}",
        f"engine_file={engine_path.name}",
        f"tracks_file={tracks_path.name}",
    ]
    if info["layout_name"] == "embedded":
        layout = INTERNAL_LAYOUTS[stem.casefold()]
        lines += [
            "asset_layout=embedded_initial_load",
            "segment_end=exclusive",
            f"engine_segments={format_segments(info['engine_segments'])}",
            f"tracks_segments={format_segments(info['track_segments'])}",
            f"tracks_load_address=0x{layout['track_segments'][0][0]:04X}",
            f"track_table_address=0x{layout['track_table_address']:04X}",
            f"track_stream_address=0x{layout['track_stream_address']:04X}",
            f"engine_extracted_size=0x{len(info['engine']):04X}",
            f"tracks_extracted_size=0x{len(info['tracks']):04X}",
        ]
    gameinfo = read_sidecar(source, ".gameinfo")
    trackinfo = read_sidecar(source, ".trackinfo")
    if gameinfo:
        lines += ["", "[gameinfo]", gameinfo]
    if trackinfo:
        lines += ["", "[trackinfo]", trackinfo]
    text_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract KSS engine and banked track resources."
    )
    parser.add_argument(
        "directory",
        type=Path,
        nargs="?",
        default=Path("vigamup"),
        help="directory containing KSS files (default: vigamup)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="output directory (default: DIRECTORY/extracted)",
    )
    args = parser.parse_args()
    directory = args.directory
    output_dir = args.output_dir or directory / "extracted"
    sources = sorted(
        path
        for path in directory.iterdir()
        if path.is_file() and path.suffix.casefold() == ".kss"
    )
    if not sources:
        parser.error(f"no .kss files found in {directory}")
    output_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    for source in sources:
        try:
            write_one(source, output_dir)
            print(f"extracted {source.name}")
        except ValueError as error:
            failures += 1
            print(f"error: {error}")
    if failures:
        return 1
    print(f"wrote {len(sources)} KSS asset sets to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
