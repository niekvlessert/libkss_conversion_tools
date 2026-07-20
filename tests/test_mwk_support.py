#!/usr/bin/env python3
"""Reference tests for the native MSX-DOS2 MWK loader contract."""
from __future__ import annotations

import struct
import unittest
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class DecodedKit:
    headers: dict[int, bytes]
    samples: dict[int, bytes]


def decode_mwk(data: bytes) -> DecodedKit:
    """Python model of msx/KSPMWK.asm for synthetic fixtures."""
    pos = 0

    def take(size: int) -> bytes:
        nonlocal pos
        if size < 0 or pos + size > len(data):
            raise ValueError("truncated MWK")
        result = data[pos : pos + size]
        pos += size
        return result

    if take(5) != b"MBMS\x10":
        raise ValueError("bad MWK signature")
    version = take(1)[0]
    if version not in (0x0C, 0x0D):
        raise ValueError("unsupported MWK version")
    edit_mode = version == 0x0C
    take(3)  # Declared sample size; chunk bounds remain authoritative.
    wave_count = take(1)[0]
    if wave_count > 48:
        raise ValueError("invalid wave count")
    flags = take(64)
    take(wave_count * 25)
    if edit_mode:
        take(wave_count * 16)

    headers: dict[int, bytes] = {}
    samples: dict[int, bytes] = {}
    sample_address = 0x200300
    for tone, flag in enumerate(flags):
        if not flag & 1:
            continue
        if edit_mode:
            take(16)
        source = take(13)
        output = bytearray(12)
        output[1] = (sample_address >> 8) & 0xFF
        output[2] = sample_address & 0xFF
        if flag & 0x20:
            output[0] = (flag & 0xC0) | source[12]
            sample_length = 0
        else:
            output[0] = (flag & 0xC0) | ((sample_address >> 16) & 0x3F)
            sample_length = source[11] | (source[12] << 8)
        output[3:12] = source[2:11]
        headers[tone] = bytes(output)
        if sample_length:
            sample = take(sample_length)
            samples[sample_address] = sample
            sample_address += sample_length
            if sample_address > 0x400000:
                raise ValueError("MWK exceeds 2 MiB sample RAM image")
    return DecodedKit(headers, samples)


def sample_header(tag: int, sample: bytes) -> bytes:
    # Bytes 2..10 are copied to the OPL4 header; 11..12 contain byte length.
    body = bytes(((tag + index) & 0xFF) for index in range(11))
    return body + struct.pack("<H", len(sample))


def make_mwk(
    version: int,
    flags: list[int],
    records: list[tuple[bytes, bytes]],
    *,
    wave_count: int = 0,
) -> bytes:
    if len(flags) != 64:
        raise ValueError("flags must contain 64 entries")
    output = bytearray(b"MBMS\x10")
    output.append(version)
    output += b"\x00\x00\x00"
    output.append(wave_count)
    output += bytes(flags)
    output += bytes(wave_count * 25)
    if version == 0x0C:
        output += bytes(wave_count * 16)
    record_iter = iter(records)
    for flag in flags:
        if not flag & 1:
            continue
        if version == 0x0C:
            output += bytes(16)
        header, sample = next(record_iter)
        output += header
        if not flag & 0x20:
            output += sample
    try:
        next(record_iter)
    except StopIteration:
        return bytes(output)
    raise ValueError("too many records")


class MwkDecodeTests(unittest.TestCase):
    def test_runtime_0d_preserves_inactive_header_slots(self) -> None:
        flags = [0] * 64
        flags[0] = 0x01
        flags[2] = 0x81
        sample0 = b"abc"
        sample2 = b"de"
        kit = make_mwk(
            0x0D,
            flags,
            [
                (sample_header(0x10, sample0), sample0),
                (sample_header(0x30, sample2), sample2),
            ],
        )
        decoded = decode_mwk(kit)
        self.assertEqual(set(decoded.headers), {0, 2})
        self.assertEqual(decoded.headers[0][0:3], bytes((0x20, 0x03, 0x00)))
        self.assertEqual(decoded.headers[2][0:3], bytes((0xA0, 0x03, 0x03)))
        self.assertEqual(decoded.samples[0x200300], sample0)
        self.assertEqual(decoded.samples[0x200303], sample2)

    def test_edit_0c_skips_patch_and_per_tone_edit_records(self) -> None:
        flags = [0] * 64
        flags[7] = 0x41
        sample = b"wave"
        kit = make_mwk(
            0x0C,
            flags,
            [(sample_header(0x50, sample), sample)],
            wave_count=2,
        )
        decoded = decode_mwk(kit)
        self.assertEqual(decoded.headers[7][0:3], bytes((0x60, 0x03, 0x00)))
        self.assertEqual(decoded.samples[0x200300], sample)

    def test_rom_header_does_not_consume_or_advance_sample_ram(self) -> None:
        flags = [0] * 64
        flags[0] = 0xA1  # active + ROM/header-only + 16-bit mode bits
        flags[1] = 0x01
        rom_source = bytearray(sample_header(0x70, b"ignored"))
        rom_source[12] = 0x15
        sample = b"xy"
        kit = make_mwk(
            0x0D,
            flags,
            [(bytes(rom_source), b""), (sample_header(0x20, sample), sample)],
        )
        decoded = decode_mwk(kit)
        self.assertEqual(decoded.headers[0][0], 0x95)
        self.assertEqual(decoded.headers[1][0:3], bytes((0x20, 0x03, 0x00)))
        self.assertEqual(decoded.samples, {0x200300: sample})

    def test_truncated_sample_is_rejected(self) -> None:
        flags = [0] * 64
        flags[0] = 1
        kit = make_mwk(
            0x0D,
            flags,
            [(sample_header(0, b"1234"), b"1234")],
        )
        with self.assertRaisesRegex(ValueError, "truncated"):
            decode_mwk(kit[:-1])

    def test_unsupported_revision_is_rejected(self) -> None:
        flags = [0] * 64
        kit = bytearray(make_mwk(0x0D, flags, []))
        kit[5] = 0x0B
        with self.assertRaisesRegex(ValueError, "unsupported"):
            decode_mwk(bytes(kit))


class SourceIntegrationTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[1]

    def test_player_resolves_song_aux_to_mwk(self) -> None:
        source = (self.ROOT / "msx" / "KSPPLAYER.asm").read_text()
        self.assertIn("SONG.aux is either FFFFFFFFH", source)
        self.assertIn("ksp_match_mwk:", source)
        self.assertIn("ld      (ksp_mwk_id),hl", source)
        self.assertIn("call    ksp_prepare_work_segment", source)

    def test_loader_embeds_page2_uploader(self) -> None:
        source = (self.ROOT / "msx" / "KSPDOS2.asm").read_text()
        self.assertIn("ld      hl,mwk_uploader_blob", source)
        self.assertIn("incbin  'KSPDOS2_MWK.raw'", source)

    def test_compact_ksp_uses_direct_mwk_streaming(self) -> None:
        loader = (self.ROOT / "msx" / "KSPDOS2.asm").read_text()
        uploader = (self.ROOT / "msx" / "KSPMWK.asm").read_text()
        self.assertIn("stage_ksp_file:", loader)
        self.assertIn("ld      a,2", loader)
        self.assertIn("stage_read_size", loader)
        self.assertIn("ksp_mwk_read_direct_byte", uploader)
        self.assertIn("ld      c,READ", uploader)
        self.assertIn("ld      de,0x4000", uploader)

    def test_mwk_reader_does_not_overwrite_its_page2_uploader(self) -> None:
        uploader = (self.ROOT / "msx" / "KSPMWK.asm").read_text()
        direct_fill = uploader.split("ksp_mwk_direct_fill:", 1)[1].split(
            "ksp_mwk_direct_fill_ok:", 1
        )[0]
        self.assertIn("RUNTIME_STAGE_TABLE+1", direct_fill)
        self.assertNotIn("RUNTIME_KSS_TABLE+2", direct_fill)

    def test_compact_reader_has_dos_backed_cache_and_decode_gather(self) -> None:
        loader = (self.ROOT / "msx" / "KSPDOS2.asm").read_text()
        player = (self.ROOT / "msx" / "KSPPLAYER.asm").read_text()
        self.assertIn("ksp_direct_helper:", loader)
        self.assertIn("ksp_direct_cache_segment:", loader)
        self.assertIn("ksp_direct_decode_stream:", loader)
        self.assertIn("call    ksp_copy_raw_song_loop", loader)
        self.assertIn("ld      ix,(RUNTIME_DIRECT_HELPER)", player)
        self.assertIn("cp      2\n        jr      z,ksp_engine_stream_ok", player)
        self.assertIn("cp      2\n        jr      z,ksp_song_stream_ok", player)

    def test_uploader_uses_moonsound_wave_ports_and_rw_mode(self) -> None:
        source = (self.ROOT / "msx" / "KSPMWK.asm").read_text()
        self.assertIn("ld      e,0x11", source)
        self.assertIn("ld      e,0x10", source)
        self.assertIn("out     (0x7E),a", source)
        self.assertIn("out     (0x7F),a", source)
        self.assertIn('defm    "MBMS"', source)


if __name__ == "__main__":
    unittest.main()
