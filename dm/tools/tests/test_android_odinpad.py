"""AYN's built-in controller (OdinPad.java with android/app/src/jvmtest's OdinPadJvmTest): the "Odin Controller" of the Thor and the Odin handhelds is found by
its vendor and product (or AYN's name for it), and its face buttons go by label unless the system's Controller style is Xbox; and GameActivity asks it for every
device it adds. The JVM part is skipped without a JDK. Each mutation of OdinPad must make the JVM test fail; one that cannot be applied exactly once is a fault of
this test, reported as such."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src/main/java/dev/nxnative/android'
SRC = JAVA / 'OdinPad.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/OdinPadJvmTest.java'

MUTATIONS = [
    ('VENDOR = 0x2020', 'VENDOR = 0x2021', 'another vendor id'),
    ('PRODUCT_THOR = 0x0111', 'PRODUCT_THOR = 0x0112', 'another product id'),
    ('return vendor == VENDOR && (product == PRODUCT_THOR || NAME.equals(name));', 'return product == PRODUCT_THOR || NAME.equals(name);', 'the vendor is not checked'),
    ('(product == PRODUCT_THOR || NAME.equals(name))', '(product == PRODUCT_THOR && NAME.equals(name))', 'product and name are both needed'),
    ('(product == PRODUCT_THOR || NAME.equals(name))', '(product == PRODUCT_THOR)', 'the name is ignored'),
    ('(product == PRODUCT_THOR || NAME.equals(name))', '(NAME.equals(name))', 'the product is ignored'),
    ('NAME.equals(name)', 'NAME.equalsIgnoreCase(name)', 'the name is matched loosely'),
    ('return is(vendor, product, name) && !STYLE_XBOX', 'return !STYLE_XBOX', 'every pad goes by label'),
    ('&& !STYLE_XBOX.equals(', '&& STYLE_XBOX.equals(', 'the style test is inverted'),
    ('&& !STYLE_XBOX.equals(style == null ? "" : style.trim());', ';', 'the style is ignored'),
    ('style == null ? "" : style.trim()', 'style == null ? "" : style', 'the value is not trimmed'),
    ('STYLE_XBOX = "1"', 'STYLE_XBOX = "0"', 'the default style is taken for Xbox'),
    ('STYLE_PROPERTY = "persist.sys.gamepad.type"', 'STYLE_PROPERTY = "persist.sys.gamepad"', 'another property'),
    ('NAME = "Odin Controller"', 'NAME = "Odin"', 'another name'),
]
NEUTRAL = ('/** The face buttons of this device go by label', '/** The face buttons of this pad go by label')


def swap(text, needle, replacement):
    if text.count(needle) != 1:
        raise RuntimeError('SETUP-ERROR: needle occurs %d times: %r' % (text.count(needle), needle))
    changed = text.replace(needle, replacement)
    if changed == text:
        raise RuntimeError('SETUP-ERROR: the mutation changes nothing: %r' % needle)
    return changed


def jvm(source_text):
    """(stage, returncode, output) of compiling OdinPad (as given) with its test and running it."""
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'OdinPad.java'
        src.write_text(source_text, encoding='utf-8')
        c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, str(src), str(TEST)], capture_output=True, text=True)
        if c.returncode:
            return 'javac', c.returncode, c.stderr
        r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.OdinPadJvmTest'], capture_output=True, text=True)
        return 'java', r.returncode, r.stdout + r.stderr


class Wiring(unittest.TestCase):
    def test_activity(self):
        t = (JAVA / 'GameActivity.java').read_text(encoding='utf-8')
        self.assertIn('Native.deviceAdded(p.id, p.name, odinLabelled(p.id))', t)   # every device added is asked
        self.assertNotIn('Native.deviceAdded(p.id, p.name, false)', t)
        self.assertIn('OdinPad.labelled(d.getVendorId(), d.getProductId(), d.getName(), OdinPad.style())', t)

    def test_setting_still_wins(self):
        t = (REPO / 'dm/host/android/input_android.c').read_text(encoding='utf-8')
        self.assertIn('g_labelled[p] = e ? atoi(e) : labelled;', t)   # DM_PAD_LABELLED over what the launcher found


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class OdinPadJvm(unittest.TestCase):
    def test_jvm_and_mutations(self):
        good = SRC.read_text(encoding='utf-8')
        stage, rc, out = jvm(good)
        self.assertEqual((stage, rc, out.strip()), ('java', 0, 'ok'), out)
        survived = []
        for needle, replacement, what in MUTATIONS:
            stage, rc, out = jvm(swap(good, needle, replacement))
            if stage == 'javac':
                raise RuntimeError('SETUP-ERROR: mutant %r does not compile:\n%s' % (what, out))
            if rc == 0:
                survived.append(what)
        self.assertEqual(survived, [], 'mutations the JVM test did not catch')
        stage, rc, out = jvm(swap(good, *NEUTRAL))
        self.assertEqual((stage, rc, out.strip()), ('java', 0, 'ok'), 'a comment-only change must still pass: ' + out)


if __name__ == '__main__':
    unittest.main()
