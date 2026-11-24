"""Pictures a game saves to the console album (AlbumFiles.java with android/app/src/jvmtest's AlbumFilesJvmTest, AlbumExport.java): which files of the album folder are
finished pictures and the gallery folder they go to, on the JVM; and that GameActivity gives the host DM_ALBUM_DIR (dm/host/hle_album.c), keeps it from the player's lines
and starts the copy into MediaStore. The JVM part is skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src/main/java/dev/nxnative/android'
SRC = [JAVA / 'AlbumFiles.java']
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/AlbumFilesJvmTest.java'


class Wiring(unittest.TestCase):
    def test_activity(self):
        t = (JAVA / 'GameActivity.java').read_text(encoding='utf-8')
        self.assertIn('File album = AlbumExport.dir(this, id);', t)
        self.assertIn('"DM_ALBUM_DIR=" + album', t)
        self.assertIn('"DM_AOC_DIR", "DM_ALBUM_DIR",', t)   # a path the launcher sets: refused from env.txt and the game's lines
        # the watch starts once the host has its environment (Native.start), so its notes reach the game's log
        self.assertLess(t.index('int r = Native.start('), t.index('AlbumExport.watch(this, album, bootName, id);'))

    def test_export(self):
        t = (JAVA / 'AlbumExport.java').read_text(encoding='utf-8')
        for s in ('MediaStore.MediaColumns.IS_PENDING, 1', 'MediaStore.MediaColumns.IS_PENDING, 0', 'MediaStore.MediaColumns.RELATIVE_PATH, rel',
                  'MediaStore.VOLUME_EXTERNAL_PRIMARY', 'FileObserver.CREATE | FileObserver.MOVED_TO', 'AlbumFiles.pending(dir)', 'AlbumFiles.exportable(name)',
                  'private static FileObserver observer;'):
            self.assertIn(s, t)
        m = (REPO / 'android/app/src/main/AndroidManifest.xml').read_text(encoding='utf-8')
        self.assertNotIn('WRITE_EXTERNAL_STORAGE', m)   # MediaStore rows of the app's own need no permission (minSdk 31)

    def test_host(self):
        h = (REPO / 'dm/host/hle_album.c').read_text(encoding='utf-8')
        self.assertIn('getenv("DM_ALBUM_DIR")', h)
        self.assertIn('errno == ENOSYS', h)   # Android's shared storage has no hard links (ENOSYS): the rename fallback publishes there
        self.assertNotIn('CoreGraphics', h)   # one PNG writer for every platform (zlib)
        self.assertNotIn('_ZN2nn5album', (REPO / 'dm/host/hle_offline.c').read_text(encoding='utf-8'))


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class AlbumFilesJvm(unittest.TestCase):
    def test_jvm(self):
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, *map(str, SRC), str(TEST)], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.AlbumFilesJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
