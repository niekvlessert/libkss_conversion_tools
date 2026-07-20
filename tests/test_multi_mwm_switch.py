#!/usr/bin/env python3
"""Source-level regression tests for native MSX-DOS2 MWM switching.

These tests deliberately do not need the private MBWave engine or an MSX
assembler. They protect the hand-off contract that was broken in MWMSWITCH.KSP:
Space selects the next SONG, all MoonSound voices are actually reset through
the MoonSound ports, and the selected SONG's optional MWK is uploaded again.
"""
from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PLAYER = ROOT / "msx" / "KSPPLAYER.asm"
LOADER = ROOT / "msx" / "KSPDOS2.asm"
BUILD = ROOT / "tools" / "build_msx_dos2_ksp.py"


def block(text: str, start: str, end: str) -> str:
    first = text.index(start)
    last = text.index(end, first)
    return text[first:last]


class MultiMwmSwitchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.player = PLAYER.read_text(encoding="utf-8")
        cls.runtime = block(
            cls.player, "ksp_page2_runtime:\n", "ksp_page2_runtime_end:\n"
        )
        cls.switch = block(cls.player, "ksp_switch_song:\n", "ksp_finish_exit:\n")
        cls.silence = block(
            cls.player, "ksp_page2_begin_silence:\n", "ksp_page2_silence_done:\n"
        )

    def test_space_and_cursor_matrix_bits(self) -> None:
        # Row 8: Space=0, Left=4, Right=7.
        self.assertIn("and     0x91", self.runtime)
        self.assertIn("bit     0,b", self.runtime)
        self.assertIn("bit     7,b", self.runtime)
        self.assertNotIn("and     0x09", self.runtime)

    def test_switch_fully_rebuilds_selected_song(self) -> None:
        expected_calls = [
            "call    ksp_parse_directory",
            "call    ksp_prepare_work_segment",
            "call    ksp_materialize",
            "call    ksp_clear_work_segment",
            "call    ksp_install_engine_patches",
            "call    ksp_install_page2_runtime",
        ]
        positions = [self.switch.index(call) for call in expected_calls]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("ld      (song_number),a", self.switch)
        self.assertIn("jp      0xB800", self.switch)

    def test_track_count_is_reinstalled(self) -> None:
        install = block(
            self.player,
            "ksp_install_page2_runtime:\n",
            "; This block is copied to B800H.",
        )
        self.assertIn("ld      a,(song_number)", install)
        self.assertIn("ld      a,(ksp_track_count)", install)
        self.assertRegex(
            self.player,
            re.compile(
                r"ksp_match_song:.*?cp\s+'G'.*?"
                r"ld\s+a,\(ksp_track_count\).*?inc\s+a.*?"
                r"ld\s+\(ksp_track_count\),a",
                re.S,
            ),
        )

    def test_moonsound_ports_are_not_msx_audio_ports(self) -> None:
        # Default MoonSound FM is C4-C7; wave is 7E/7F.
        for wrong in ("out     (0xC0)", "out     (0xC1)", "out     (0xC2)", "out     (0xC3)"):
            self.assertNotIn(wrong, self.silence)
        for required in (
            "out     (0xC4)",
            "out     (0xC5)",
            "out     (0xC6)",
            "out     (0xC7)",
            "out     (0x7E)",
            "out     (0x7F)",
        ):
            self.assertIn(required, self.silence)

    def test_all_wave_slots_are_damped(self) -> None:
        self.assertIn("ld      d,0x50", self.silence)
        self.assertIn("ld      b,24", self.silence)
        self.assertIn("ld      d,0x68", self.silence)
        self.assertIn("ld      a,0x40", self.silence)

    def test_global_mixer_is_restored_before_init(self) -> None:
        pre_init = block(
            self.runtime, "ksp_page2_work_segment:\n", "ksp_page2_init_target:\n"
        )
        self.assertIn("ld      a,0xF8", pre_init)
        self.assertIn("ld      a,0xF9", pre_init)
        self.assertGreaterEqual(pre_init.count("out     (0x7F),a"), 2)


    def test_resident_helpers_are_relocated_after_bootstrap(self) -> None:
        layout = (ROOT / "msx" / "PLAYER_LAYOUT.inc").read_text(encoding="utf-8")
        expected = {
            "PUT_P0_DISPATCH": "0xD9C0",
            "PUT_P1_DISPATCH": "0xD9C4",
            "PUT_P2_DISPATCH": "0xD9C8",
            "CUSTOM_ENASLT": "0xD9D0",
            "RST28_SAVED": "0xD910",
            "HTIMI_INSTALLED": "0xD91D",
        }
        for symbol, address in expected.items():
            self.assertRegex(
                layout,
                re.compile(rf"^{symbol}:\s+equ\s+{address}$", re.M),
            )
        build = BUILD.read_text(encoding="utf-8")
        self.assertIn('player_labels["player_end"] > retained_state_start', build)
        self.assertIn('player_labels["player_end"] > dispatch_start', build)
        self.assertIn("scratch_end > dispatch_start", build)

    def test_mwk_uploader_is_built_and_staged(self) -> None:
        self.assertTrue((ROOT / "msx" / "KSPMWK.asm").is_file())
        loader = LOADER.read_text(encoding="utf-8")
        build = BUILD.read_text(encoding="utf-8")
        self.assertIn("mwk_uploader_blob", loader)
        self.assertIn("KSPDOS2_MWK.raw", loader)
        self.assertIn("KSPMWK.asm", build)
        self.assertIn("KSPMWK_SYMBOLS.inc", build)


if __name__ == "__main__":
    unittest.main()
