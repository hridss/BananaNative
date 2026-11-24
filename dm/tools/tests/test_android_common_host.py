"""The common host library (experimental, Stage 2 of android/PORTING.md's common host): the same names in the CMake option, the Gradle property, the version script and the
profile library. NX_COMMON=ON (Gradle -PnxCommon=true) builds one libnxhost.so, one libnxprofile_<title ID>.so per game and one libnxlaunch.so; the default (OFF) still builds
libnxhost_<id>.so and libnxlaunch_<id>.so per game. These checks read the sources, so they need no NDK."""
import fnmatch
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / 'android/host'
PROFILE = REPO / 'android/profile'
APP = REPO / 'android/app'


def read(p): return Path(p).read_text(encoding='utf-8')


def map_globals(text):
    """The names a GNU version script lists under global:"""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    m = re.search(r'global:(.*?)(?:local:|\})', text, flags=re.S)
    return [n.strip() for n in (m.group(1) if m else '').split(';') if n.strip()]


class CommonHost(unittest.TestCase):
    def test_version_script_exports_the_setter(self):
        names = map_globals(read(HOST / 'libnxhost.map'))
        self.assertTrue(any(fnmatch.fnmatchcase('nxhost_set_profile', n) for n in names), f'libnxhost.map does not export nxhost_set_profile: {names}')
        self.assertIn('nxhost_main', names)

    def test_profile_lib_exports_only_what_it_needs(self):
        # it is a constructor plus the profile it passes on: any version script of android/profile lists no global symbol but (at most) none at all
        for m in PROFILE.glob('*.map'):
            self.assertEqual(map_globals(read(m)), [], f'{m.name}: a profile library exports nothing (its constructor runs at load time)')
        c = read(PROFILE / 'profile.c')
        self.assertRegex(c, r'__attribute__\(\(constructor\)\)[^\n]*nxhost_set_profile\(&dm_game\)', 'profile.c: the constructor must call nxhost_set_profile(&dm_game)')
        self.assertRegex(c, r'visibility\("hidden"\)\)\)\s*const struct dm_profile dm_game', 'profile.c: dm_game must be hidden')

    def test_store_defines_the_setter_and_the_storage(self):
        s = HOST / 'nxprofile_store.c'
        if not s.is_file(): self.skipTest('android/host/nxprofile_store.c is not there yet')
        t = read(s)
        self.assertIn('nxhost_set_profile', t)
        self.assertRegex(t, r'\bdm_game\b')
        self.assertIn('memcpy', t)

    def test_empty_game_profile_header(self):
        for h in HOST.glob('*/game_profile.h'):
            body = re.sub(r'/\*.*?\*/|//[^\n]*', '', read(h), flags=re.S).strip()
            self.assertEqual(body, '', f'{h.relative_to(REPO)}: the common build\'s game_profile.h is empty (the profile comes from nxhost_set_profile)')

    def test_cmake_option_and_gradle_property_agree(self):
        cm = read(HOST / 'CMakeLists.txt')
        self.assertRegex(cm, r'option\(\s*NX_COMMON\b[^\n]*\bON\s*\)', 'android/host/CMakeLists.txt: option NX_COMMON, default ON')
        self.assertIn('NX_COMMON_PROFILE', cm)
        gr = read(APP / 'build.gradle')
        self.assertIn('nxCommon', gr)
        self.assertRegex(gr, r'-DNX_COMMON=\$\{nxCommon \? \'ON\' : \'OFF\'\}', 'build.gradle passes -DNX_COMMON=ON when -PnxCommon=true')
        self.assertRegex(gr, r"buildConfigField\s+'boolean',\s*'NX_COMMON'")
        self.assertRegex(gr, r"gradleProperty\('nxCommon'\)\.getOrElse\('true'\)", 'nxCommon defaults to true')

    def test_libs_have_the_agreed_names(self):
        cm = read(HOST / 'CMakeLists.txt') + read(APP / 'src/main/cpp/CMakeLists.txt') + read(APP / 'build.gradle')
        for n in ('nxhost', 'nxprofile_', 'nxlaunch'): self.assertIn(n, cm, n)
        java = read(APP / 'src/main/java/dev/nxnative/android/Native.java')
        self.assertRegex(java, r'loadLibrary\("nxhost"\)')
        self.assertRegex(java, r'loadLibrary\("nxprofile_"\s*\+')
        self.assertRegex(java, r'loadLibrary\("nxlaunch"\)')
        self.assertRegex(java, r'loadLibrary\("nxlaunch_"\s*\+', 'the old flavour keeps nxlaunch_<id>')
        self.assertIn('NX_COMMON', java)
        order = [java.index(s) for s in ('loadLibrary("nxhost")', 'loadLibrary("nxprofile_"', 'loadLibrary("nxlaunch")')]
        self.assertEqual(order, sorted(order), 'common load order: nxhost, nxprofile_<id>, nxlaunch')


if __name__ == '__main__':
    unittest.main()
