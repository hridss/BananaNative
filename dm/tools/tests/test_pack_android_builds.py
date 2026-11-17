"""A game with several registered builds (Mario Kart World 1.0.0 and 1.8.0: the same game, another module set and another RomFS) packs from a dump of either:
scripts/pack_android.py dump_modules takes the modules of the build row a dump matches, and optional_keys reads a profile's shader_optional.tsv, the shader keys only some builds have."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_android', REPO / 'scripts/pack_android.py')
pa = importlib.util.module_from_spec(spec); spec.loader.exec_module(pa)

A, B, C, D = ('%032x' % n for n in (1, 2, 3, 4))   # build IDs


class Profile:   # what dump_modules asks of a game profile
    dir = Path('.')
    def __init__(self, modules, builds): self._m, self._b = modules, builds
    def words(self, key): return self._m.split()
    def builds(self): return self._b


def nso(path, build_id):
    path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(build_id) + bytes(0x10))


class DumpModules(unittest.TestCase):
    G = Profile('main subsdk0 subsdk1 sdk', {'v0': {'display': '1.0.0', 'main': A, 'subsdk0': B, 'sdk': C},
                                             'v524288': {'display': '1.8.0', 'main': D, 'subsdk0': C, 'subsdk1': B, 'sdk': A}})

    def exefs(self, files):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for n, i in files.items(): nso(d / n, i)
        return d

    def test_every_module_there(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': D, 'subsdk0': C, 'subsdk1': B, 'sdk': A})), ['main', 'subsdk0', 'subsdk1', 'sdk'])

    def test_build_with_one_module_less(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'subsdk0': B, 'sdk': C})), ['main', 'subsdk0', 'sdk'])

    def test_unregistered_set_takes_the_modules_there(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'subsdk0': D, 'sdk': C})), ['main', 'subsdk0', 'sdk'])   # ids of no row: Try anyway

    def test_no_main_is_left_to_the_caller(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'subsdk0': D, 'sdk': C})), ['main', 'subsdk0', 'subsdk1', 'sdk'])   # the caller says what is missing


class OptionalKeys(unittest.TestCase):
    def test_file(self):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (d / 'shader_optional.tsv').write_text('# comment\n00aa\t1.0.0\n11bb\t1.8.0\n')
        g = Profile('main', {}); g.dir = d
        self.assertEqual(pa.optional_keys(g), {'00aa', '11bb'})
        g.dir = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.assertEqual(pa.optional_keys(g), set())


class SplitAbsent(unittest.TestCase):
    """split_shaders: the keys of the profile's map that the dump lacks and shader_optional.tsv names go to <package>/_absent.tsv; any other missing key stops the pack"""

    def run_split(self, dump_keys, optional):
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        prof = root / 'profile'; prof.mkdir()
        row = lambda k: f'cs_x.bin\t0\tcompute\t{k}\tsrc\n'
        (prof / 'shader_map.tsv').write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in ('k1', 'k2', 'k3')))
        if optional: (prof / 'shader_optional.tsv').write_text(''.join(f'{k}\t1.0.0\n' for k in optional))
        g = Profile('main', {}); g.dir = prof; g.id = '0000000000000000'
        def fake_run(cmd, env=None):   # shader_split.py: <romfs> <main> <pkg> <map> <facts> <pairs> <pso_pairs>
            Path(cmd[4]).mkdir(parents=True)
            Path(cmd[5]).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in dump_keys))
            for f in cmd[6:]: Path(f).write_text('')
            return type('R', (), {'returncode': 0})()
        args = type('A', (), {'shader_python': None})()
        pkg = root / 'pkg'
        old = pa.subprocess.run, pa.shader_python, pa.child_env
        pa.subprocess.run, pa.shader_python, pa.child_env = fake_run, lambda *a, **k: 'python', lambda **k: {}
        try: pa.split_shaders(args, g, root / 'v', pkg, (root / 'romfs', root / 'exefs'))
        finally: pa.subprocess.run, pa.shader_python, pa.child_env = old
        return pkg

    def test_optional_keys_are_listed(self):
        pkg = self.run_split(['k1', 'k2'], ['k3'])
        self.assertEqual((pkg / '_absent.tsv').read_text(), 'k3\n')

    def test_complete_dump_has_no_list(self):
        pkg = self.run_split(['k1', 'k2', 'k3'], ['k3'])
        self.assertFalse((pkg / '_absent.tsv').exists())

    def test_other_missing_key_stops(self):
        with self.assertRaises(pa.Error) as e: self.run_split(['k1'], ['k3'])
        self.assertIn('lack 1 of the 3', str(e.exception))

    def test_absent_optional(self):
        """pack_shaders builds the library without the keys of _absent.tsv only when the profile's shader_optional.tsv names every one"""
        pkg = self.run_split(['k1', 'k2'], ['k3'])
        g = Profile('main', {}); g.dir = pkg.parent / 'profile'
        self.assertEqual(pa.absent_optional(g, pkg), {'k3'})
        (pkg / '_absent.tsv').write_text('k2\nk3\n')
        self.assertEqual(pa.absent_optional(g, pkg), set(), 'k2 is not optional')
        (pkg / '_absent.tsv').unlink()
        self.assertEqual(pa.absent_optional(g, pkg), set())


class SplitOtherVersions(unittest.TestCase):
    """Tables that are the union of two versions (game.tsv shader_versions), a dump of one of them (may_lack): the rows tagged with the other listed version may be lacking"""

    def union(self, versions='v100 v200'):
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        prof = root / 'profile'; prof.mkdir()
        srcs = {'k1': '/Common/a.ptcl#BNSH@0x1', 'k2': 'main#BNSH@0x2', 'k3': '/v200/Common/a.ptcl#BNSH@0x3', 'k4': 'main@v200#BNSH@0x4'}
        row = lambda k: f'cs_x.bin\t0\tcompute\t{k}\t{srcs[k]}\n'
        (prof / 'shader_map.tsv').write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in srcs))
        g = Profile('main', {}); g.dir = prof; g.id = '0000000000000000'; g.words = lambda key: versions.split() if key == 'shader_versions' else ['main']
        def split(dump_keys, ver):
            def fake_run(cmd, env=None):
                Path(cmd[4]).mkdir(parents=True)
                Path(cmd[5]).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in dump_keys))
                for f in cmd[6:]: Path(f).write_text('')
                return type('R', (), {'returncode': 0})()
            args = type('A', (), {'shader_python': None, 'pack_ver': ver})()
            pkg = root / f'pkg{len(list(root.iterdir()))}'
            old = pa.subprocess.run, pa.shader_python, pa.child_env
            pa.subprocess.run, pa.shader_python, pa.child_env = fake_run, lambda *a, **k: 'python', lambda **k: {}
            import contextlib, io
            said = io.StringIO()
            try:
                with contextlib.redirect_stdout(said): pa.split_shaders(args, g, root / 'v', pkg, (root / 'romfs', root / 'exefs'))
            finally: pa.subprocess.run, pa.shader_python, pa.child_env = old
            return pkg, said.getvalue()
        return g, split

    def test_a_dump_of_one_listed_version_builds(self):
        g, split = self.union()
        self.assertEqual(pa.other_versions(g, 100), ['v200'])
        self.assertEqual(pa.other_version_keys(g, 100), {'k3', 'k4'})
        pkg, said = split(['k1', 'k2'], 100)
        self.assertEqual((pkg / '_absent.tsv').read_text(), 'k3\nk4\n')
        self.assertIn('shaders: 2 of 4 shaders in this app\'s table belong to other versions of the game (v200): not needed for v100\n', said)
        self.assertEqual(pa.absent_optional(g, pkg, 100), {'k3', 'k4'}, 'the library is built without them, as for shader_optional.tsv')
        self.assertEqual(pa.absent_optional(g, pkg), set(), 'not without the dump\'s version')

    def test_an_unlisted_version_is_refused_as_before(self):
        g, split = self.union()
        self.assertEqual(pa.other_version_keys(g, 300), set())
        with self.assertRaises(pa.Error) as e: split(['k1', 'k2'], 300)
        self.assertIn('lack 2 of the 4', str(e.exception)); self.assertIn('the dump is of another version or incomplete', str(e.exception))

    def test_all_versions_keys_are_required_otherwise(self):
        g, split = self.union()
        pkg, said = split(['k1', 'k2', 'k3', 'k4'], 100)   # nothing lacking
        self.assertFalse((pkg / '_absent.tsv').exists()); self.assertNotIn('other versions', said)
        with self.assertRaises(pa.Error) as e: split(['k1', 'k3', 'k4'], 100)   # an untagged shader missing: the versions do not explain it
        self.assertIn('lack 1 of the 4', str(e.exception))
        with self.assertRaises(pa.Error): split(['k1', 'k3', 'k4'], 200)   # the tags do not say what a later version dropped
        g1, split1 = self.union('v100')   # one version: every key
        self.assertEqual(pa.other_version_keys(g1, 100), set())
        with self.assertRaises(pa.Error): split1(['k1', 'k2'], 100)
        g2, _ = self.union('v100 v2000')   # v200 is not v2000
        self.assertEqual(pa.other_version_keys(g2, 100), set())

    def test_mario_kart_8_deluxe_is_two_versions(self):   # the profile as it is: 135 of 29,785 rows are 4.0.1's own
        g = pa.game.load('0100152000022000')
        self.assertEqual(pa.shader_versions(g), ['v1441792', 'v1507328'])
        self.assertEqual(len(pa.other_version_keys(g, 1441792)), 135)
        self.assertEqual(len(pa.map_keys(g.dir / 'shader_map.tsv')), 29785)
        self.assertEqual(pa.other_version_keys(g, 1507328), set(), 'what 4.0.1 dropped of 4.0.0 is not in the tags: its shader_optional.tsv names those')
        named = {l.split('\t')[0] for l in (g.dir / 'shader_optional.tsv').read_text().splitlines() if '4.0.1 only' in l}
        self.assertEqual(pa.other_version_keys(g, 1441792), named, 'the tags agree with the profile\'s own list of 4.0.1\'s shaders')

    def test_super_mario_bros_wonder_names_both_sides(self):   # its shader_optional.tsv already names each version's own keys: the tags agree with it
        g = pa.game.load('010015100B514000')
        tagged = pa.other_version_keys(g, 0)
        named = {l.split('\t')[0] for l in (g.dir / 'shader_optional.tsv').read_text().splitlines() if '(v327680) only' in l}
        self.assertEqual(tagged, named); self.assertEqual(len(tagged), 3457)


class MarioKartWorldOptional(unittest.TestCase):
    """Mario Kart World 1.8.0 as update 917504 (Forgejo #50): its split lacks only the profile's optional key, which the profile's map has"""

    def test_profile(self):
        d = REPO / 'dm/games/0400C3F00006E000'
        g = Profile('main', {}); g.dir = d
        self.assertEqual(pa.optional_keys(g), {'797e77c2ec4341c5'})
        self.assertLessEqual(pa.optional_keys(g), pa.map_keys(d / 'shader_map.tsv'))


if __name__ == '__main__':
    unittest.main()
