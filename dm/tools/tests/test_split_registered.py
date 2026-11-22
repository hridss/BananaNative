"""A key found in more than one place keeps the copy at the place the profile's shader_map.tsv names (shader_split.py registered_sources, merge_results, and main's copies in last_entries):
the game's copy still takes an add-on's place first, a registered add-on copy never displaces the game's, the stage must match, a saved or captured copy never takes a
key, and every copy is checked against the first. After upstream nxnative f41613f78 (#3130, Fast Fusion). Synthetic entries only."""
import contextlib
import io
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import fuzzlib
ss = fuzzlib.import_shader_split()

KEY = 'aaaaaaaaaaaaaaaa'


def copy(where, st='vs', sig='same'):
    """A merged entry as the scan gives it: (where, stage, control, code, (shader_sig, record_problem, kept))"""
    return (where, st, b'ctl', b'code', (sig, None, True))


def result(where, st='vs', sig='same'):
    return (where, {KEY: copy(where, st, sig)}, set(), {})


GAME = result('/game.pak#1')
DLC = result('/aoc/0/dlc.pak#1')


class MergeResults(unittest.TestCase):
    def test_first_copy_without_a_map(self):
        later = result('/later.pak#1')
        self.assertEqual(ss.merge_results([GAME, later])[0][KEY][0], '/game.pak#1')
        self.assertEqual(ss.merge_results([GAME, later], {})[0][KEY][0], '/game.pak#1')

    def test_registered_source_across_files(self):
        later = result('/blocker_full.w3d#1')
        keys, _, per_src = ss.merge_results([GAME, later], {(KEY, 'vertex'): '/blocker_full.w3d#1'})
        self.assertEqual(keys[KEY][0], '/blocker_full.w3d#1')
        self.assertEqual(sum(per_src.values()), 1)

    def test_registered_source_must_match_the_stage(self):
        later = result('/later.pak#1')
        self.assertEqual(ss.merge_results([GAME, later], {(KEY, 'pixel'): '/later.pak#1'})[0][KEY][0], '/game.pak#1')
        self.assertEqual(ss.merge_results([GAME, later], {(KEY, 'vertex'): '/later.pak#1'})[0][KEY][0], '/later.pak#1')

    def test_registered_dlc_cannot_displace_game_copy(self):
        earlier = result('/aoc/0/earlier.pak#1')
        reg = {(KEY, 'vertex'): '/aoc/0/dlc.pak#1'}
        for order in ([earlier, DLC, GAME], [GAME, earlier, DLC]):
            with self.subTest(order=[r[0] for r in order]):
                self.assertEqual(ss.merge_results(order, reg)[0][KEY][0], '/game.pak#1')

    def test_registered_dlc_copy_among_dlc_copies(self):
        earlier = result('/aoc/0/earlier.pak#1')
        self.assertEqual(ss.merge_results([earlier, DLC], {(KEY, 'vertex'): '/aoc/0/dlc.pak#1'})[0][KEY][0], '/aoc/0/dlc.pak#1')

    def test_play_capture_never_takes_a_key(self):
        play = result('/glslc_play/run.glslc')
        self.assertEqual(ss.merge_results([GAME, play], {(KEY, 'vertex'): '/glslc_play/run.glslc'})[0][KEY][0], '/game.pak#1')

    def test_every_copy_is_checked_before_one_is_chosen(self):
        registered, conflict = result('/registered.pak#1'), result('/conflict.pak#1', sig='other')
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(SystemExit, 'control section or code area differs'):
            ss.merge_results([GAME, registered, conflict], {(KEY, 'vertex'): '/registered.pak#1'})


class MainCopies(unittest.TestCase):
    """main's copies (last_entries): the last copy, unless the map names another place of the key with the same stage"""
    CTL, CODE = b'\x00' * 0x40, b'\x01' * 0x40

    def found(self, *wheres, st='vs'):
        return [(w, st, self.CTL, self.CODE) for w in wheres]

    def test_last_copy_without_a_map(self):
        got = ss.last_entries(self.found('main#BNSH@0x0#var0', 'main#BNSH@0x0#var2'))
        self.assertEqual([v[0] for v in got.values()], ['main#BNSH@0x0#var2'])

    def test_registered_variation(self):
        key = ss.skey(self.CTL, self.CODE)
        got = ss.last_entries(self.found('main#BNSH@0x0#var0', 'main#BNSH@0x0#var1', 'main#BNSH@0x0#var2'), {(key, 'vertex'): 'main#BNSH@0x0#var1'})
        self.assertEqual(got[key][0], 'main#BNSH@0x0#var1')

    def test_registered_place_of_another_stage_is_ignored(self):
        key = ss.skey(self.CTL, self.CODE)
        got = ss.last_entries(self.found('main#a', 'main#b'), {(key, 'pixel'): 'main#a'})
        self.assertEqual(got[key][0], 'main#b')


if __name__ == '__main__':
    unittest.main()
