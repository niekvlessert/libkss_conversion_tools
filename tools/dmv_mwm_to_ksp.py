#!/usr/bin/env python3
"""Extract DMV MWM assets and build one KSP file per song."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_VETERANS_DIR = REPOSITORY_ROOT / "dutch_moonsound_veterans"

# These offsets are the on-disk DMV1 MWM layout: the six-byte MBMS signature
# is included in the offsets below.
MWM_SONG_NAME_OFFSET = 0xE2
MWM_SONG_NAME_SIZE = 50
MWM_WAVE_KIT_OFFSET = 0x114
MWM_WAVE_KIT_SIZE = 8
MWM_WAVE_NUMBERS_OFFSET = 0x82
MWM_WAVE_NUMBERS_SIZE = 48

# DISKEXEC.BAS contains records of the form:
#   "title","KIT.MWK" or "*","SONG.MWM","author",...
# The menu record is more reliable than the editable MWM header for the
# samplepack selection.
TRACK_RECORD_RE = re.compile(
    rb'"([^"\r\n]*)","([^"\r\n]*)","([A-Za-z0-9_]+\.MWM)"', re.IGNORECASE
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Extract .MWM/.MWK assets from the Dutch MoonSound Veterans ZIP "
            "files and create one .ksp file per MWM song."
        )
    )
    parser.add_argument(
        "--veterans-dir",
        type=Path,
        default=DEFAULT_VETERANS_DIR,
        help="directory containing DMV*.zip (default: dutch_moonsound_veterans)",
    )
    parser.add_argument(
        "--mwm-dir",
        type=Path,
        help="asset extraction directory (default: VETERANS_DIR/mwm)",
    )
    parser.add_argument(
        "--ksp-dir",
        type=Path,
        help="KSP output directory (default: VETERANS_DIR/ksp)",
    )
    parser.add_argument(
        "--mbwave2ksp",
        type=Path,
        help="mbwave2ksp executable (default: build-moonsound/mbwave2ksp or build/mbwave2ksp)",
    )
    parser.add_argument(
        "--zx0",
        action="store_true",
        help="ZX0-compress ENGN and SONG chunks (disabled by default)",
    )
    return parser.parse_args()


def clean_text(data: bytes) -> str:
    return " ".join(data.decode("ascii", "replace").replace("\x00", " ").split())


def archive_collection(archive: Path, member_name: str) -> str:
    parts = [part for part in PurePosixPath(member_name).parts if part not in ("", ".")]
    if len(parts) > 1:
        return parts[0]
    return archive.stem


def extract_assets(veterans_dir: Path, mwm_dir: Path) -> dict[tuple[str, str], tuple[str | None, str]]:
    archives = sorted(
        veterans_dir.glob("*.zip"),
        key=lambda path: (0 if path.stem.upper() == "DMVFT" else 1, path.name.upper()),
    )
    if not archives:
        raise RuntimeError(f"no ZIP archives found in {veterans_dir}")

    extracted = 0
    duplicates = 0
    conflicts = 0
    track_metadata: dict[tuple[str, str], tuple[str | None, str]] = {}
    for archive in archives:
        with zipfile.ZipFile(archive) as bundle:
            for info in bundle.infolist():
                if info.is_dir():
                    continue
                member = info.filename
                upper_name = member.upper()
                data = bundle.read(info)
                if upper_name.endswith((".BAS", ".BIN")):
                    collection = archive_collection(archive, member)
                    for title_raw, kit_raw, song_raw in TRACK_RECORD_RE.findall(data):
                        song_name = song_raw.decode("ascii", "replace").upper()
                        title = clean_text(title_raw)
                        kit_text = clean_text(kit_raw)
                        if kit_text.upper().endswith(".MWK"):
                            kit_text = kit_text[:-4]
                        kit_name = None if kit_text == "*" else kit_text
                        track_metadata.setdefault((collection, song_name), (kit_name, title))

                if not (
                    upper_name.endswith(".MWM")
                    or upper_name.endswith(".MWK")
                    or upper_name.endswith("/WAVEDRVR.BIN")
                    or upper_name == "WAVEDRVR.BIN"
                ):
                    continue

                collection = archive_collection(archive, member)
                basename = PurePosixPath(member).name
                target = mwm_dir / collection / basename
                target.parent.mkdir(parents=True, exist_ok=True)

                if target.exists():
                    if target.read_bytes() == data:
                        duplicates += 1
                    else:
                        conflicts += 1
                        print(
                            f"[extract] keeping {target} (different duplicate in "
                            f"{archive.name}:{member})",
                            file=sys.stderr,
                        )
                    continue

                target.write_bytes(data)
                extracted += 1
                print(f"[extract] {archive.name}:{member} -> {target}")

    print(
        f"[extract] {extracted} assets extracted, {duplicates} duplicates skipped, "
        f"{conflicts} conflicts skipped"
    )
    print(f"[extract] {len(track_metadata)} menu track records found")
    return track_metadata


def find_case_insensitive(directory: Path, filename: str) -> list[Path]:
    wanted = filename.casefold()
    return sorted(
        path for path in directory.rglob("*") if path.is_file() and path.name.casefold() == wanted
    )


def choose_driver(song: Path, all_drivers: list[Path]) -> Path | None:
    sibling = find_case_insensitive(song.parent, "WAVEDRVR.BIN")
    if sibling:
        return sibling[0]
    return all_drivers[0] if all_drivers else None


def song_metadata(song: Path) -> tuple[str, str, bool]:
    data = song.read_bytes()
    if len(data) < MWM_WAVE_KIT_OFFSET + MWM_WAVE_KIT_SIZE:
        raise ValueError("file is too short for the MWM header")
    if data[:6] not in (b"MBMS\x10\x08", b"MBMS\x10\x07"):
        raise ValueError("not an MWM/MBMS file")

    kit_name = clean_text(data[MWM_WAVE_KIT_OFFSET : MWM_WAVE_KIT_OFFSET + MWM_WAVE_KIT_SIZE])
    title = clean_text(data[MWM_SONG_NAME_OFFSET : MWM_SONG_NAME_OFFSET + MWM_SONG_NAME_SIZE])
    wave_end = MWM_WAVE_NUMBERS_OFFSET + MWM_WAVE_NUMBERS_SIZE
    needs_mwk = wave_end <= len(data) and any(
        wave >= 176 for wave in data[MWM_WAVE_NUMBERS_OFFSET:wave_end]
    )
    return kit_name, title, needs_mwk


def find_samplepack(song: Path, kit_name: str, mwm_dir: Path) -> Path | None:
    if not kit_name or kit_name.upper() == "NONE":
        return None
    wanted = kit_name.casefold()
    wanted_83 = wanted[:8]
    local = sorted(
        path
        for path in song.parent.iterdir()
        if path.is_file()
        and path.suffix.casefold() == ".mwk"
        and (path.stem.casefold() == wanted or path.stem.casefold()[:8] == wanted_83)
    )
    if local:
        return local[0]
    global_matches = sorted(
        path
        for path in mwm_dir.rglob("*")
        if path.is_file()
        and path.suffix.casefold() == ".mwk"
        and (path.stem.casefold() == wanted or path.stem.casefold()[:8] == wanted_83)
    )
    if len(global_matches) == 1:
        return global_matches[0]
    return None


def find_mbwave2ksp(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else [
        REPOSITORY_ROOT / "build-moonsound" / "mbwave2ksp",
        REPOSITORY_ROOT / "build" / "mbwave2ksp",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    names = ", ".join(str(candidate) for candidate in candidates if candidate)
    raise RuntimeError(f"mbwave2ksp was not found; tried: {names}")


def create_ksp_files(
    mwm_dir: Path,
    ksp_dir: Path,
    mbwave2ksp: Path,
    track_metadata: dict[tuple[str, str], tuple[str | None, str]],
    use_zx0: bool = False,
) -> int:
    songs = sorted(
        (path for path in mwm_dir.rglob("*") if path.is_file() and path.suffix.upper() == ".MWM"),
        key=lambda path: str(path).casefold(),
    )
    drivers = find_case_insensitive(mwm_dir, "WAVEDRVR.BIN")
    if not songs:
        raise RuntimeError(f"no MWM songs found in {mwm_dir}")
    if not drivers:
        raise RuntimeError(f"no WAVEDRVR.BIN found in {mwm_dir}")

    failures = 0
    for song in songs:
        relative = song.relative_to(mwm_dir)
        output = (ksp_dir / relative).with_suffix(".ksp")
        output.parent.mkdir(parents=True, exist_ok=True)
        driver = choose_driver(song, drivers)
        assert driver is not None

        try:
            header_kit_name, header_title, needs_mwk = song_metadata(song)
        except ValueError as error:
            print(f"[skip] {song}: {error}", file=sys.stderr)
            failures += 1
            continue

        menu_kit_name, menu_title = track_metadata.get(
            (song.parent.name, song.name.upper()), (header_kit_name, header_title)
        )
        samplepack = find_samplepack(song, menu_kit_name or "", mwm_dir)
        menu_has_explicit_pack = (song.parent.name, song.name.upper()) in track_metadata and menu_kit_name
        if menu_has_explicit_pack and samplepack is None:
            print(
                f"[warn] {song}: menu samplepack {menu_kit_name}.MWK was not found; "
                "creating KSP without it",
                file=sys.stderr,
            )
        if not menu_has_explicit_pack and needs_mwk and samplepack is None:
            print(
                f"[warn] {song}: required samplepack "
                f"{header_kit_name or '(unnamed)'}.MWK was not found; creating KSP without it",
                file=sys.stderr,
            )

        command = [
            str(mbwave2ksp),
            "--driver",
            str(driver),
            "--song",
            str(song),
            "--output",
            str(output),
        ]
        if use_zx0:
            command.append("--zx0")
        if samplepack:
            command.extend(["--mwk", str(samplepack)])
        title = menu_title or header_title
        if title:
            command.extend(["--title", title])

        print(
            f"[ksp] {song} -> {output}"
            f" (kit={menu_kit_name or 'NONE'}, mwk={samplepack or 'none'})"
        )
        result = subprocess.run(command, text=True, capture_output=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.returncode != 0:
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            print(f"[fail] mbwave2ksp failed for {song}", file=sys.stderr)
            failures += 1

    print(f"[ksp] created {len(songs) - failures} KSP files; {failures} failures")
    return failures


def main() -> int:
    args = parse_args()
    veterans_dir = args.veterans_dir.resolve()
    mwm_dir = (args.mwm_dir or veterans_dir / "mwm").resolve()
    ksp_dir = (args.ksp_dir or veterans_dir / "ksp").resolve()

    try:
        mbwave2ksp = find_mbwave2ksp(args.mbwave2ksp.resolve() if args.mbwave2ksp else None)
        mwm_dir.mkdir(parents=True, exist_ok=True)
        track_metadata = extract_assets(veterans_dir, mwm_dir)
        return create_ksp_files(
            mwm_dir, ksp_dir, mbwave2ksp, track_metadata, args.zx0
        )
    except (OSError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
