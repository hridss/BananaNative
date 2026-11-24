"""Execute the movie budget and its existing consumers without a game dump.

The v131072 constructor's one-picture mode limits both packet feeding and
picture advancement. The two-picture mode is already implemented by the game.
Unicorn is optional, as in test_astral_fps.py.
"""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
from typing import Any


def optional_emulator() -> tuple[Any, Any]:
    # This execution dependency is optional in the project's normal environment.
    try:
        return importlib.import_module('unicorn'), importlib.import_module('unicorn.arm64_const')
    except ImportError:
        return None, None


uc, arm = optional_emulator()

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
BID = 'e0f3ccfaf50065ae4ea01eb5aeb13b1a'
SITE = 0x3b5d084
spec = importlib.util.spec_from_file_location('oot_movie_patches', REPO / 'dm/games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class MovieBudget(unittest.TestCase):
    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        self.code, self.obj = 0x100000, 0x200000
        self.u.mem_map(self.code, 0x1000)
        self.u.mem_map(self.obj, 0x1000)
        self.cut = next(c for c in patch.CUTS[BID] if c[0] == SITE)

    def execute(self, code):
        self.u.mem_write(self.code, code)
        self.u.emu_start(self.code, self.code + len(code))

    def budget(self, changed, mode):
        self.u.reg_write(arm.UC_ARM64_REG_W27, mode)
        self.u.reg_write(arm.UC_ARM64_REG_W28, 1)
        self.u.reg_write(arm.UC_ARM64_REG_X19, self.obj)
        self.u.reg_write(arm.UC_ARM64_REG_W26, 16)
        # cmp w27,#2; original/patched selection; str w8,[x19,#0x20];
        # lsl w8,w8,#1; stp w8,w26,[x19,#0x50]. The constructor allocates
        # the packet buffers from this same selection; do not bypass only a
        # consumer clamp and leave its feeder restricted to one packet.
        self.execute(bytes.fromhex('7f0b0071' + self.cut[2 if changed else 1]
                                   + '682200b908791f53686a0a29'))
        budget = struct.unpack('<I', self.u.mem_read(self.obj + 0x20, 4))[0]
        buffers, capacity = struct.unpack('<II', self.u.mem_read(self.obj + 0x50, 8))
        self.assertEqual((buffers, capacity), (2 * budget, 16))
        return budget

    def advance(self, due, budget, audio_clock=False):
        self.u.reg_write(arm.UC_ARM64_REG_W8, due)
        self.u.reg_write(arm.UC_ARM64_REG_W9, budget)
        # Existing consumers at main+0x3b5cb28 (elapsed update) and
        # main+0x3b5e3fc (audio-clock catch-up). Both retain their bounds.
        self.execute(bytes.fromhex('1f01096b' + ('0831891a' if audio_clock else '14b1891a')))
        return self.u.reg_read(arm.UC_ARM64_REG_W8 if audio_clock else arm.UC_ARM64_REG_W20)

    def test_existing_two_picture_mode_and_matching_buffers(self):
        for mode in (0, 1, 2, 4):
            self.assertEqual(self.budget(False, mode), 1 if mode < 2 else 2)
            self.assertEqual(self.budget(True, mode), 2)

    def test_sixty_fps_movie_under_thirty_fps_presentation(self):
        old = self.budget(False, 1)
        new = self.budget(True, 1)
        for audio_clock in (False, True):
            # 9,252 pictures at 60 Hz last 154.2 seconds. At 30 Hz the
            # old budget advances only half the movie during its soundtrack.
            self.assertEqual(4626 * self.advance(2, old, audio_clock), 4626)
            self.assertEqual(4626 * self.advance(2, new, audio_clock), 9252)

    def test_sixty_fps_playback_and_bounded_catchup(self):
        budget = self.budget(True, 1)
        for audio_clock in (False, True):
            self.assertEqual(self.advance(1, budget, audio_clock), 1)
            self.assertEqual(self.advance(60, budget, audio_clock), 2)


if __name__ == '__main__':
    unittest.main()
