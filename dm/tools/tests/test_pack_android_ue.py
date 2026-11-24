"""scripts/pack_android.py's Unreal Engine shader code archives: a pack whose RomFS has no ue_shaders folder beside it (a .nup or .nx, or a dump folder the NSP script did not
decode) has uepak.py decode them into the work folder and names them to the split (DM_UE_SHADERS); shaders that need the game's Oodle stop the pack with what to do.
Also nro_collect.scan, which reads its job's range of a file rather than a mapping of the whole file (a file of an .nx has none: its fileno() copies all of it)."""
import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_android', REPO / 'scripts/pack_android.py')
assert spec and spec.loader
pack_android = importlib.util.module_from_spec(spec); spec.loader.exec_module(pack_android)
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import nro_collect   # noqa: E402
import uepak   # noqa: E402


class UeShaders(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        t = Path(self.tmp.name)
        self.rom, self.exe, self.pkg = t / 'game/romfs', t / 'game/exefs', t / 'work/android-shaders/pkg'
        self.rom.mkdir(parents=True); self.exe.mkdir(parents=True); self.pkg.parent.mkdir(parents=True)
        (self.rom / 'Content').mkdir(); (self.rom / 'Content/x.pak').write_bytes(bytes(0x400))   # a pak by name (not Unreal's: no footer magic)
        env = {k: v for k, v in os.environ.items() if k != 'DM_UE_SHADERS'}
        p = mock.patch.dict(os.environ, env, clear=True); p.start(); self.addCleanup(p.stop)

    def run_with(self, rc, out=''):
        with mock.patch.object(pack_android.subprocess, 'run', return_value=subprocess.CompletedProcess([], rc, out, '')) as run:
            return pack_android.ue_shaders(sys.executable, self.rom, self.exe, self.pkg), run

    def test_decoded_into_the_work_folder(self):
        got, run = self.run_with(0, 'decoded: a.ushaderbytecode in x\n')
        self.assertEqual(got, self.pkg.parent / 'ue_shaders')
        self.assertEqual(run.call_args[0][0][2:], ['decode', str(self.rom), str(self.exe / 'main'), str(self.pkg.parent / 'ue_shaders')])

    def test_no_archives_leaves_the_split_alone(self):
        self.assertIsNone(self.run_with(0, 'no shader code archive in the paks: nothing to decode\n')[0])

    def test_oodle_stops_the_pack(self):
        with self.assertRaisesRegex(pack_android.Error, 'Oodle.*DM_UE_SHADERS'): self.run_with(3, '[RED] x: 5 shaders ... need the game\'s Oodle\n')

    def test_nsp_script_folder_or_setting_kept(self):
        (self.rom.parent / 'ue_shaders').mkdir()
        got, run = self.run_with(0, 'decoded: a in x\n')
        self.assertIsNone(got); run.assert_not_called()
        (self.rom.parent / 'ue_shaders').rmdir()
        with mock.patch.dict(os.environ, {'DM_UE_SHADERS': 'x'}):
            got, run = self.run_with(0, 'decoded: a in x\n')
        self.assertIsNone(got); run.assert_not_called()

    def test_no_pak_no_decode(self):
        (self.rom / 'Content/x.pak').unlink()
        got, run = self.run_with(0, 'decoded: a in x\n')
        self.assertIsNone(got); run.assert_not_called()

    def test_uepak_decode_without_paks(self):
        self.assertEqual(uepak.decode(str(self.rom), str(self.exe / 'main'), str(self.pkg.parent / 'ue_shaders')), [])
        self.assertFalse((self.pkg.parent / 'ue_shaders').exists())

    def test_uepak_decode_refuses_oodle(self):
        with mock.patch.object(uepak, 'unreal_paks', return_value=[str(self.rom / 'a.pak')]), mock.patch.object(uepak, 'io_shader_tocs', return_value=[]), \
             mock.patch.object(uepak, 'job', return_value=([], 7)), mock.patch.object(uepak.os.path, 'getsize', return_value=1):
            with self.assertRaises(uepak.NeedsHost): uepak.decode(str(self.rom), str(self.exe / 'main'), str(self.pkg.parent / 'ue_shaders'))
        self.assertFalse((self.pkg.parent / 'ue_shaders').exists())


class ScanRange(unittest.TestCase):
    def test_across_job_edges(self):
        zstd = nro_collect.zstd_mod()
        def nro(n, tag):
            b = bytearray(hashlib.sha256(tag).digest() * (n // 32)); b[0x10:0x14] = b'NRO0'; struct.pack_into('<I', b, 0x18, n); return bytes(b)
        a, z = nro(0x2000, b'a'), nro(0x40000, b'z'); zf = zstd.compress(z)
        data = bytearray(0x300000); oa, oz = 0x100000 - 0x100, 0x200000 - 0x10   # a stored NRO across the first job's end, a frame starting near the second's
        data[oa:oa + len(a)] = a; data[oz:oz + len(zf)] = zf
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'blob'); Path(p).write_bytes(data)
            want, found = {hashlib.sha256(a).digest(), hashlib.sha256(z).digest()}, {}
            for s in range(0, len(data), 0x100000):
                for h, _, off, how, body in nro_collect.scan((p, s, min(len(data), s + 0x100000 + 0x40), want)): found[h] = (off, how, body)
        self.assertEqual(found[hashlib.sha256(a).digest()], (oa, 'stored', a))
        self.assertEqual(found[hashlib.sha256(z).digest()], (oz, 'zstd', z))


if __name__ == '__main__': unittest.main()
