#!/usr/bin/env python3
"""Report which MWK sample records an MWM song can use."""

from __future__ import annotations

import argparse
import hashlib
import sys
from dataclasses import dataclass
from pathlib import Path


MWM_HEADER_SIZE = 278
MWM_WAVE_NUMBERS_OFFSET = 124
MWM_WAVE_NUMBERS_SIZE = 48
MWM_START_WAVES_OFFSET = 100
MWM_WAVE_KIT_OFFSET = 270
MWM_WAVE_KIT_SIZE = 8
MWM_EDIT_POSITION_TABLE_SIZE = 220

MWK_MAX_TONES = 64
MWK_PATCH_SIZE = 25
MWK_PATCH_PARTS = 8
MWK_SAMPLE_HEADER_SIZE = 13


@dataclass(frozen=True)
class Sample:
    tone: int
    flags: int
    size: int
    header_offset: int
    data_offset: int


@dataclass(frozen=True)
class Patch:
    parts: tuple[tuple[int, int, int], ...]


@dataclass(frozen=True)
class MwmInfo:
    path: Path
    signature: bytes
    kit_name: str
    used_wave_indices: tuple[int, ...]
    custom_patches: tuple[int, ...]


@dataclass(frozen=True)
class MwkInfo:
    path: Path
    signature: bytes
    declared_sample_size: int
    samples: tuple[Sample, ...]
    patches: tuple[Patch, ...]


def load_fix_file(path: Path) -> dict[str, str]:
    fixes: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 2:
            raise ValueError(
                f"{path}:{line_number}: expected MWM_FILENAME MWK_FILENAME"
            )
        mwm_name = fields[0].casefold()
        mwk_name = fields[1]
        if mwm_name in fixes and fixes[mwm_name] != mwk_name:
            raise ValueError(f"{path}:{line_number}: conflicting fix for {fields[0]}")
        fixes[mwm_name] = mwk_name
    return fixes


def clean_ascii(data: bytes) -> str:
    return " ".join(data.decode("ascii", "replace").replace("\x00", " ").split())


def require(data: bytes, end: int, description: str) -> None:
    if end > len(data):
        raise ValueError(f"truncated {description}")


def parse_mwm(path: Path) -> MwmInfo:
    data = path.read_bytes()
    require(data, 6, "MWM signature")
    signature = data[:6]
    if signature not in (b"MBMS\x10\x07", b"MBMS\x10\x08"):
        raise ValueError(f"{path} is not an MWM file (signature {signature!r})")

    # Edit-mode MWM files have a complete 220-byte position table before the
    # header. Runtime MWM files put the header immediately after the signature.
    header_offset = 6 + (MWM_EDIT_POSITION_TABLE_SIZE if signature.endswith(b"\x07") else 0)
    require(data, header_offset + MWM_HEADER_SIZE, "MWM header")
    wave_offset = header_offset + MWM_WAVE_NUMBERS_OFFSET
    kit_offset = header_offset + MWM_WAVE_KIT_OFFSET
    require(data, wave_offset + MWM_WAVE_NUMBERS_SIZE, "MWM wave table")
    require(data, kit_offset + MWM_WAVE_KIT_SIZE, "MWM wave-kit name")

    waves = data[wave_offset : wave_offset + MWM_WAVE_NUMBERS_SIZE]
    song_length = data[header_offset]

    # Decode the compact 16-row pattern representation used by the MWM
    # player. Wave-change events select one of the 48 wave table slots.
    if signature.endswith(b"\x07"):
        position_table = data[6 : 6 + MWM_EDIT_POSITION_TABLE_SIZE]
        cursor = header_offset + MWM_HEADER_SIZE
    else:
        cursor = header_offset + MWM_HEADER_SIZE
        require(data, cursor + song_length + 1, "MWM position table")
        position_table = data[cursor : cursor + song_length + 1]
        cursor += song_length + 1
    positions = position_table[: song_length + 1]
    max_pattern = max(positions, default=0)
    require(data, cursor + 2 * (max_pattern + 1), "MWM pattern offsets")
    pattern_offsets = [
        int.from_bytes(data[cursor + 2 * i : cursor + 2 * i + 2], "little")
        for i in range(max_pattern + 1)
    ]
    cursor += 2 * (max_pattern + 1)
    patterns: list[bytes | None] = [None] * (max_pattern + 1)
    pattern_count = 0
    while pattern_count <= max_pattern:
        require(data, cursor + 3, "MWM pattern group header")
        group_size = int.from_bytes(data[cursor : cursor + 2], "little")
        group_count = data[cursor + 2]
        cursor += 3
        if group_count == 0:
            raise ValueError(f"{path} contains an empty MWM pattern group")
        require(data, cursor + group_size, "MWM pattern group")
        group = data[cursor : cursor + group_size]
        cursor += group_size
        if pattern_count > max_pattern:
            break
        base_offset = pattern_offsets[pattern_count]
        for index in range(group_count):
            pattern_index = pattern_count + index
            if pattern_index > max_pattern:
                break
            relative = pattern_offsets[pattern_index] - base_offset
            if relative < 0 or relative >= group_size:
                continue
            if index < group_count - 1 and pattern_index + 1 <= max_pattern:
                pattern_size = pattern_offsets[pattern_index + 1] - pattern_offsets[pattern_index]
            else:
                pattern_size = group_size - relative
            if pattern_size > 0 and relative + pattern_size <= group_size:
                patterns[pattern_index] = group[relative : relative + pattern_size]
        pattern_count += group_count

    used_wave_indices = {
        wave_index - 1
        for wave_index in data[
            header_offset + MWM_START_WAVES_OFFSET :
            header_offset + MWM_START_WAVES_OFFSET + 24
        ]
        if 1 <= wave_index <= MWM_WAVE_NUMBERS_SIZE
    }
    for pattern_index in positions:
        pattern = patterns[pattern_index]
        if pattern is None:
            raise ValueError(f"{path} has no data for MWM pattern {pattern_index}")
        cursor = 0
        for _step in range(16):
            require(pattern, cursor + 1, f"MWM pattern {pattern_index} row")
            event = pattern[cursor]
            cursor += 1
            if event == 0xFF:
                continue
            require(pattern, cursor + 3, f"MWM pattern {pattern_index} masks")
            masks = pattern[cursor : cursor + 3]
            cursor += 3
            if 98 <= event <= 145:
                used_wave_indices.add(event - 98)
            for mask in masks:
                for bit in range(8):
                    if mask & (0x80 >> bit):
                        require(pattern, cursor + 1, f"MWM pattern {pattern_index} event")
                        channel_event = pattern[cursor]
                        cursor += 1
                        if 98 <= channel_event <= 145:
                            used_wave_indices.add(channel_event - 98)

    used_wave_indices_tuple = tuple(sorted(used_wave_indices))
    custom_patches = tuple(
        sorted({waves[index] for index in used_wave_indices_tuple if waves[index] >= 176})
    )
    return MwmInfo(
        path=path,
        signature=signature,
        kit_name=clean_ascii(data[kit_offset : kit_offset + MWM_WAVE_KIT_SIZE]),
        used_wave_indices=used_wave_indices_tuple,
        custom_patches=custom_patches,
    )


def parse_mwk(path: Path) -> MwkInfo:
    data = path.read_bytes()
    require(data, 10, "MWK header")
    signature = data[:6]
    if signature not in (b"MBMS\x10\x0C", b"MBMS\x10\x0D"):
        raise ValueError(f"{path} is not an MWK file (signature {signature!r})")

    edit_mode = signature.endswith(b"\x0C")
    declared_sample_size = int.from_bytes(data[6:9], "little")
    nr_of_waves = data[9]
    if nr_of_waves > 48:
        raise ValueError(f"{path} declares {nr_of_waves} patches; maximum is 48")

    cursor = 10
    require(data, cursor + MWK_MAX_TONES, "MWK tone table")
    tone_info = data[cursor : cursor + MWK_MAX_TONES]
    cursor += MWK_MAX_TONES

    patches: list[Patch] = []
    for _ in range(nr_of_waves):
        require(data, cursor + MWK_PATCH_SIZE, "MWK patch table")
        cursor += 1  # transpose byte
        parts = []
        for _ in range(MWK_PATCH_PARTS):
            next_note = data[cursor]
            tone = data[cursor + 1]
            tone_note = data[cursor + 2]
            parts.append((next_note, tone, tone_note))
            cursor += 3
        patches.append(Patch(tuple(parts)))

    if edit_mode:
        edit_table_size = nr_of_waves * 16
        require(data, cursor + edit_table_size, "MWK edit table")
        cursor += edit_table_size

    samples: list[Sample] = []
    for tone, flags in enumerate(tone_info):
        if not flags & 0x01:
            continue
        if edit_mode:
            require(data, cursor + 16, "MWK per-sample edit data")
            cursor += 16
        header_offset = cursor
        require(data, cursor + MWK_SAMPLE_HEADER_SIZE, "MWK sample header")
        header = data[cursor : cursor + MWK_SAMPLE_HEADER_SIZE]
        cursor += MWK_SAMPLE_HEADER_SIZE
        # Bit 5 marks a ROM/header-only tone. It has no sample payload in MWK.
        size = 0 if flags & 0x20 else int.from_bytes(header[11:13], "little")
        data_offset = cursor
        require(data, cursor + size, f"MWK sample data for tone {tone}")
        samples.append(Sample(tone, flags, size, header_offset, data_offset))
        cursor += size

    return MwkInfo(path, signature, declared_sample_size, tuple(samples), tuple(patches))


def reachable_tones(patch: Patch) -> set[int]:
    """Return tone records selectable for notes 0..95 by the MWK player ABI."""

    tones: set[int] = set()
    for note in range(96):
        part_index = 0
        while part_index < len(patch.parts):
            next_note, tone, _tone_note = patch.parts[part_index]
            if note < next_note or next_note == 0:
                tones.add(tone)
                break
            part_index += 1
    return tones


def format_bytes(value: int) -> str:
    return f"{value:,} B"


def find_mwk(mwm_path: Path, kit_name: str, mwk_path: Path | None,
             mwk_dir: Path | None, fixes: dict[str, str],
             fix_file: Path | None) -> Path:
    if mwk_path is not None:
        if not mwk_path.is_file():
            raise ValueError(f"specified MWK does not exist: {mwk_path}")
        return mwk_path

    full_name = "/".join(part.casefold() for part in mwm_path.parts)
    override_name = fixes.get(mwm_path.name.casefold())
    for key, value in fixes.items():
        if "/" in key and (full_name == key or full_name.endswith("/" + key)):
            if override_name is not None and override_name != value:
                raise ValueError(f"conflicting fixes match {mwm_path}")
            override_name = value
    if override_name is not None:
        override = Path(override_name)
        direct_candidates = []
        if override.is_absolute():
            direct_candidates.append(override)
        else:
            direct_candidates.append(mwm_path.parent / override)
            if mwk_dir is not None:
                direct_candidates.append(mwk_dir / override)
            if fix_file is not None:
                direct_candidates.append(fix_file.parent / override)
        for candidate in direct_candidates:
            if candidate.is_file():
                return candidate
        target = override.name.casefold()
        search_dirs = [(mwm_path.parent, False)]
        if mwk_dir is not None:
            search_dirs.append((mwk_dir, True))
        matches = [
            path
            for directory, recursive in search_dirs
            if directory.is_dir()
            for path in (directory.rglob("*") if recursive else directory.iterdir())
            if path.is_file() and path.name.casefold() == target
        ]
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            contents = {hashlib.sha256(path.read_bytes()).digest() for path in matches}
            if len(contents) == 1:
                return sorted(matches)[0]
        searched = f"{mwm_path.parent}"
        if mwk_dir is not None:
            searched += f" and {mwk_dir}"
        raise ValueError(
            f"fix file maps {mwm_path.name} to {override_name}, but it was not found; "
            f"searched {searched}"
        )

    if not kit_name or kit_name.upper() in {"NONE", "*"}:
        raise ValueError(
            f"{mwm_path} does not name an MWK; specify an MWK file or use --mwk-dir "
            "with a known kit name"
        )

    wanted = kit_name.casefold()
    wanted_83 = wanted[:8]

    def matches(directory: Path, recursive: bool) -> list[Path]:
        if not directory.is_dir():
            return []
        candidates = directory.rglob("*") if recursive else directory.iterdir()
        return sorted(
            path
            for path in candidates
            if path.is_file()
            and path.suffix.casefold() == ".mwk"
            and (path.stem.casefold() == wanted or path.stem.casefold()[:8] == wanted_83)
        )

    def collapse_identical(matches: list[Path]) -> list[Path]:
        if len(matches) <= 1:
            return matches
        contents = {hashlib.sha256(path.read_bytes()).digest() for path in matches}
        return matches[:1] if len(contents) == 1 else matches

    local_matches = collapse_identical(matches(mwm_path.parent, recursive=False))
    if len(local_matches) == 1:
        return local_matches[0]
    if len(local_matches) > 1:
        names = ", ".join(str(path) for path in local_matches)
        raise ValueError(f"multiple MWKs matching {kit_name} beside {mwm_path}: {names}")

    if mwk_dir is not None:
        directory_matches = collapse_identical(matches(mwk_dir, recursive=True))
        if len(directory_matches) == 1:
            return directory_matches[0]
        if len(directory_matches) > 1:
            names = ", ".join(str(path) for path in directory_matches)
            raise ValueError(f"multiple MWKs matching {kit_name} under {mwk_dir}: {names}")

    searched = f"{mwm_path.parent}"
    if mwk_dir is not None:
        searched += f" and {mwk_dir}"
    raise ValueError(f"could not find {kit_name}.MWK; searched {searched}")


def print_sample_group(title: str, samples: list[Sample], patch_users: dict[int, list[int]]) -> None:
    total = sum(sample.size for sample in samples)
    print(f"{title}: {len(samples)} sample(s), {format_bytes(total)} uncompressed data")
    if not samples:
        print("  none")
        return
    for sample in samples:
        users = patch_users.get(sample.tone, [])
        suffix = f"; patches {', '.join(str(p) for p in users)}" if users else ""
        storage = "ROM/header-only" if sample.flags & 0x20 else "RAM sample"
        print(f"  tone {sample.tone:02d}: {format_bytes(sample.size)} ({storage}{suffix})")


def inspect(mwm: MwmInfo, mwk_path: Path) -> None:
    mwk = parse_mwk(mwk_path)
    samples_by_tone = {sample.tone: sample for sample in mwk.samples}

    used_tones: set[int] = set()
    patch_users: dict[int, list[int]] = {}
    warnings: list[str] = []
    for patch_number in mwm.custom_patches:
        patch_index = patch_number - 176
        if patch_index >= len(mwk.patches):
            warnings.append(
                f"MWM references patch {patch_number}, but MWK has only "
                f"{len(mwk.patches)} patch records"
            )
            continue
        for tone in reachable_tones(mwk.patches[patch_index]):
            used_tones.add(tone)
            patch_users.setdefault(tone, []).append(patch_number)

    missing = sorted(tone for tone in used_tones if tone not in samples_by_tone)
    if missing:
        warnings.append(
            "MWM-selected tone records are not present in the MWK: "
            + ", ".join(str(tone) for tone in missing)
        )

    used = [samples_by_tone[tone] for tone in sorted(used_tones) if tone in samples_by_tone]
    unused = [sample for sample in mwk.samples if sample.tone not in used_tones]

    print(f"MWM: {mwm.path}")
    print(f"MWK: {mwk.path}")
    print(f"MWM wave kit name: {mwm.kit_name or '(blank)'}")
    print(
        "MWM custom patches: "
        + (", ".join(str(patch) for patch in mwm.custom_patches) if mwm.custom_patches else "none")
    )
    print(
        "MWM wave slots used: "
        + (
            ", ".join(str(index + 1) for index in mwm.used_wave_indices)
            if mwm.used_wave_indices
            else "none"
        )
    )
    print(f"MWK sample data declared: {format_bytes(mwk.declared_sample_size)}")
    print(f"MWK sample records: {len(mwk.samples)}")
    print_sample_group("Used", used, patch_users)
    print_sample_group("Unused", unused, {})
    print(
        "Total: "
        f"{len(used)} used + {len(unused)} unused = {len(mwk.samples)} sample records; "
        f"{format_bytes(sum(sample.size for sample in mwk.samples))} sample data"
    )
    print("Note: sizes are sample payload bytes only; 13-byte MWK sample headers are excluded.")
    if warnings:
        print("Warnings:", file=sys.stderr)
        for warning in warnings:
            print(f"  {warning}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Show which uncompressed MWK sample records are used by an MWM song."
    )
    parser.add_argument("mwm", type=Path, help="MWM song file")
    parser.add_argument(
        "mwk", type=Path, nargs="?", help="optional explicit MWK wave-kit file"
    )
    parser.add_argument(
        "--mwk-dir",
        type=Path,
        help="directory to search recursively when the MWK is not beside the MWM",
    )
    parser.add_argument(
        "--fix-file",
        "--fixes",
        dest="fix_file",
        type=Path,
        help="two-column MWM-to-MWK override file",
    )
    args = parser.parse_args()
    try:
        mwm = parse_mwm(args.mwm)
        fixes = load_fix_file(args.fix_file) if args.fix_file else {}
        mwk = find_mwk(args.mwm, mwm.kit_name, args.mwk, args.mwk_dir, fixes, args.fix_file)
        inspect(mwm, mwk)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
