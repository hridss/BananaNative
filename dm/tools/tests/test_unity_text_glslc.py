"""The Unity TextAsset path of the shader split: unityfs.text_assets and shader_split.unity_text_glslc read whole GLSLC outputs a game keeps in a
TextAsset (class 49) of a serialized file (FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition, Unity 2018.4: shader.switch.vfxsb). The bytes are only
decoded when the file holds the GLSLC magic, and glslc_shaders keeps only outputs that pass its checks, so a synthetic serialized file pins down that a
file without the magic is byte-identical for other games, and that only the TextAsset whose bytes hold the magic adds shaders. No game files needed."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import cpk  # noqa: E402
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
import unityfs
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

GLSLC_MAGIC = bytes.fromhex('91688619')   # the magic glslc_shaders scans for (a literal in shader_split, not a named constant)


def utf_table(name, cols, rows):
    """A CRI @UTF table: every column stored per row, (column name, type) in cols (4 u32, 6 u64, 0xa string), one dict per row"""
    size = {4: 4, 6: 8, 0xa: 4}
    strings = bytearray(b'<NULL>\0')
    def string(s):
        o = len(strings); strings.extend(s.encode() + b'\0'); return o
    name_at = string(name)
    col_bytes = b''.join(bytes([0x50 | t]) + struct.pack('>I', string(c)) for c, t in cols)
    body = bytearray()
    for r in rows:
        for c, t in cols:
            body += struct.pack('>I', string(r[c])) if t == 0xa else struct.pack('>Q' if t == 6 else '>I', r[c])
    ro = 0x18 + len(col_bytes); so = ro + len(body)
    head = struct.pack('>HHIIIHHI', 1, ro, so, so + len(strings), name_at, len(cols), sum(size[t] for _, t in cols), len(rows))
    t = head + col_bytes + bytes(body) + bytes(strings)
    return b'@UTF' + struct.pack('>I', len(t)) + t


def cpk_file(files):
    """A CRI CPK holding files (name → bytes) stored under the directory 'd', its TOC right after the header (TocOffset 0x800), the data 0x800-aligned from ContentOffset"""
    def chunk(magic, table): return magic + b'\xff\0\0\0' + struct.pack('<I', len(table)) + bytes(4) + table
    content, data, at = 0x2000, bytearray(), {}
    for nm, b in files.items():
        at[nm] = content + len(data); data += b + bytes(-len(b) % 0x800)
    rows = [{'DirName': 'd', 'FileName': nm, 'FileSize': len(b), 'ExtractSize': len(b), 'FileOffset': at[nm] - 0x800} for nm, b in files.items()]
    toc = chunk(b'TOC ', utf_table('CpkTocInfo', [('DirName', 0xa), ('FileName', 0xa), ('FileSize', 4), ('ExtractSize', 4), ('FileOffset', 6)], rows))
    head = chunk(b'CPK ', utf_table('CpkHeader', [('ContentOffset', 6), ('TocOffset', 6)], [{'ContentOffset': content, 'TocOffset': 0x800}]))
    return bytes((bytes(head.ljust(0x800, b'\0')) + toc).ljust(content, b'\0') + data)


def glslc_output():
    """A synthetic whole-GLSLC output that glslc_shaders accepts: the magic, the whole length at +0x44, one control section (its own length at +0x20,
    the code length at +0x704) and the code area right after it, a vertex shader (SPH word 0x400, shader type 1 in bits 10..13 → 'vs'), so glslc_stage
    and check both pass."""
    nctl, ncode = 0x850, 0x40            # control section length, code area length
    c = 0x100                            # the control section's offset: the only CTL_MAGIC in the output
    d = bytearray(c + nctl + ncode)
    d[:4] = GLSLC_MAGIC
    struct.pack_into('<I', d, 0x44, len(d))
    d[c:c + 4] = ss.CTL_MAGIC
    struct.pack_into('<I', d, c + 0x20, nctl)
    struct.pack_into('<I', d, c + 0x704, ncode)
    k = c + nctl                         # the code area starts right after the control section
    d[k:k + 4] = ss.GFX_MAGIC
    struct.pack_into('<I', d, k + 0x30, 0x400)
    return bytes(d)


def serialized_file(assets):
    """A synthetic serialized file (format 17, no type tree) whose objects are TextAssets. assets: [(name, script bytes)].
    The layout unityfs._tables reads: a 16-byte big-endian header + endianness byte, then little-endian metadata (version string, platform, no type tree,
    one type entry for class 49, the object table) and the object data; an asset given as bytes is an object of those raw bytes. Each TextAsset object = int32 name length + name + align to 4 + int32 script
    length + script, the two leading fields text_assets reads without a type tree."""
    blks = []
    for a in assets:
        if isinstance(a, bytes): blks.append(a); continue   # an object's raw bytes
        name, script = a
        nb = name.encode()
        b = struct.pack('<i', len(nb)) + nb
        b += b'\0' * (-len(b) % 4)
        b += struct.pack('<i', len(script)) + script
        blks.append(b)
    meta = bytearray()
    meta += b'2021.3.0\0'                # version string (read and discarded)
    meta += struct.pack('<i', 0)         # platform
    meta += b'\0'                        # no type tree
    meta += struct.pack('<i', 1)         # one type entry
    meta += struct.pack('<i', unityfs.CLASS_TEXT_ASSET)
    meta += b'\0'                        # unknown byte
    meta += struct.pack('<h', 0)         # unknown int16
    meta += bytes(16)                    # type hash
    meta += struct.pack('<i', len(assets))
    start = 0
    for i, blk in enumerate(blks):
        meta += b'\0' * (-len(meta) % 4)     # r.align() before each object table entry
        meta += struct.pack('<q', i + 1)     # path ID
        meta += struct.pack('<I', start)     # start, relative to the data offset
        meta += struct.pack('<I', len(blk))  # size
        meta += struct.pack('<i', 0)         # type index
        start += len(blk)
    doff = 20 + len(meta)
    fsz = doff + sum(len(b) for b in blks)
    head = struct.pack('>IIII', len(meta), fsz, 17, doff)
    return head + b'\0\0\0\0' + bytes(meta) + b''.join(blks)


class TextAssets(unittest.TestCase):
    def test_name_and_bytes(self):
        d = serialized_file([('vfxsb', glslc_output()), ('notes', b'hello world')])
        got = list(unityfs.text_assets(d, {}))
        self.assertEqual([(pid, name) for pid, name, _ in got], [(1, 'vfxsb'), (2, 'notes')])
        self.assertEqual(got[0][2], glslc_output())
        self.assertEqual(got[1][2], b'hello world')

    def test_objects_too_short_for_the_layout(self):
        # a name whose padding runs past the object, and an object shorter than the two length fields: left out, not read past their end
        d = serialized_file([struct.pack('<i', 1) + b'ab\0\0\0', b'\0\0', ('vfxsb', glslc_output())])
        got = list(unityfs.text_assets(d, {}))
        self.assertEqual([name for _, name, _ in got], ['vfxsb'])


class UnityTextGlslc(unittest.TestCase):
    def test_reads_only_the_textasset_that_holds_the_magic(self):
        d = serialized_file([('vfxsb', glslc_output()), ('notes', b'hello world')])
        out = ss.unity_text_glslc(d, 'resources.assets')
        self.assertEqual(len(out), 1)
        where, st, ctl, code = out[0]
        self.assertEqual(where, 'resources.assets#vfxsb#GLSLC@0x0')
        self.assertEqual(st, 'vs')
        self.assertEqual(ctl[:4], ss.CTL_MAGIC)
        self.assertEqual(code[:4], ss.GFX_MAGIC)

    def test_file_without_the_magic_is_unchanged(self):
        # the magic gate: a serialized file whose TextAssets hold no GLSLC magic yields nothing, as it did before the change
        d = serialized_file([('notes', b'hello world')])
        self.assertEqual(ss.unity_text_glslc(d, 'resources.assets'), [])

    def test_reads_unity_bundle_inside_cpk(self):
        asset = serialized_file([('vfxsb', glslc_output())])
        info = bytes(16) + struct.pack('>iIIHiqqI', 1, len(asset), len(asset), 0, 1, 0, len(asset), 0) + b'resources.assets\0'
        head = b'UnityFS\0' + struct.pack('>I', 6) + b'5.x.x\0' + b'2021.2.17f1\0'
        bundle = head + struct.pack('>qIII', len(head) + 20 + len(info) + len(asset), len(info), len(info), 0) + info + asset
        with tempfile.TemporaryDirectory() as temp, patch.object(ss, 'COMMON', {}), patch.object(ss, 'TREES', {}):
            path = str(Path(temp) / 'data.cpk')
            Path(path).write_bytes(cpk_file({'shaders.bundle': bundle}))
            rel, shaders, pairs, trees = ss.scan_file((path, 'data.cpk', tuple(cpk.files(path))))
        self.assertEqual(rel, 'data.cpk')
        self.assertEqual([row[:2] for row in shaders.values()], [
            ('/data.cpk/d/shaders.bundle:resources.assets#vfxsb#GLSLC@0x0', 'vs'),
        ])
        self.assertEqual(pairs, set())
        self.assertEqual(trees, {})


if __name__ == '__main__':
    unittest.main()
