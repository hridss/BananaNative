"""The launcher's compiling-shaders screen (android/README.md, In-game notices; PrepareScreen.java with android/app/src/jvmtest's PrepareScreenJvmTest): the host's boot
hold status "<taken> <total> <seconds left or -1>" (vka_pso.c nxhost_pso_prepare_status), the words shown, and the wiring from the host to the launcher. The JVM part is
skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/PrepareScreen.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/PrepareScreenJvmTest.java'


class Wiring(unittest.TestCase):
    def test_switches_and_status_are_wired(self):
        tsv = (REPO / 'docs/env_switches.tsv').read_text(encoding='utf-8')
        for k in ('DM_VK_PREPARE', 'DM_VK_PREPARE_SLOTS', 'DM_VK_PREPARE_MAX_S', 'DM_VK_PREPARE_SHOW_MS'):
            self.assertIn(k + '\t', tsv)
        pso = (REPO / 'dm/gfx/vk_android/vka_pso.c').read_text(encoding='utf-8')
        self.assertIn('void nxhost_pso_prepare_status(char *buf, size_t n)', pso)
        self.assertIn('void nxhost_pso_prepare_skip(void)', pso)
        c = (REPO / 'android/app/src/main/cpp/nxlaunch.c').read_text(encoding='utf-8')
        self.assertIn('Native_prepareStatus', c); self.assertIn('Native_prepareSkip', c)
        j = (REPO / 'android/app/src/main/java/dev/nxnative/android/Native.java').read_text(encoding='utf-8')
        self.assertIn('native String prepareStatus', j); self.assertIn('native void prepareSkip', j)
        self.assertIn('prepareStart();', (REPO / 'android/app/src/main/java/dev/nxnative/android/GameActivity.java').read_text(encoding='utf-8'))

    def test_the_status_format_matches(self):
        # the host writes three numbers; PrepareScreen.parse takes exactly three
        self.assertIn('"%u %u %d"', (REPO / 'dm/gfx/vk_android/vka_pso.c').read_text(encoding='utf-8'))


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class Screen(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='preparescreen_'))
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(cls.tmp / 'classes'), str(SRC), str(TEST)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def java(self, *a):
        r = subprocess.run(['java', '-Dstdout.encoding=UTF-8', '-cp', str(self.tmp / 'classes'), 'dev.nxnative.android.PrepareScreenJvmTest', *map(str, a)],
                           capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.rstrip('\n').split(' ', 1)[1]

    def test_parse(self):
        self.assertEqual(self.java('parse', '812 1875 41'), '812 1875 41')
        self.assertEqual(self.java('parse', '3 1875 -1'), '3 1875 -1')
        self.assertEqual(self.java('parse', '1900 1875 0'), '1875 1875 0')   # more taken than listed (the threads count as they take): capped
        for s in ('-', '', '812 1875', 'a b c', '5 0 3', '1 2 3 4'):
            self.assertEqual(self.java('parse', s if s else '-'), 'null', s)

    def test_words(self):
        self.assertEqual(self.java('show', '812 1875 41'), '812 of 1,875|About 45 s left|0.433')
        self.assertEqual(self.java('show', '12 1875 -1'), '12 of 1,875|Working out the time left|0.006')
        self.assertEqual(self.java('show', '1800 1875 4'), '1,800 of 1,875|Almost done|0.960')
        self.assertEqual(self.java('show', '100 1875 150'), '100 of 1,875|About 3 min left|0.053')
        self.assertEqual(self.java('show', '100 1875 89'), '100 of 1,875|About 1 min left|0.053')


if __name__ == '__main__':
    unittest.main()
