"""Needle's embedded NVN shader records, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage):
    ctl, code = bytearray(0x850), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs':
        struct.pack_into('<3I', ctl, 0x748, 8, 4, 1)
    else:
        code[:4] = split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (1 if stage == 'vs' else 5) << 10 | 1)
        if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)   # pixel shader reads generic location 0
    header = bytearray(0x90)
    struct.pack_into('<4I', header, 0x30, 0x90, 0x90 + len(ctl), len(code), len(ctl))
    return bytes(header + ctl + code), bytes(ctl), bytes(code)


def container(*stages):
    # Reflection/variant metadata can precede and separate the NVN records.
    d = bytearray(b'HNSHV002' + bytes(24) + b'variant metadata')
    for stage in stages:
        d += shader(stage)[0] + bytes(36)
    struct.pack_into('>I', d, 8, len(d))
    return bytes(d)


def lz4_chunks(b, stored=()):
    """b in 64 KB chunks: (bytes, stored length, length), LZ4 except the chunks numbered in stored"""
    import lz4.block
    out = []
    for i in range(0, max(len(b), 1), 0x10000):
        part = b[i:i + 0x10000]
        c = part if i // 0x10000 in stored else lz4.block.compress(part, store_size=False)
        out.append((c, len(c), len(part)))
    return out


def pacx403(blocks, types=b'VertexShader\0PixelShader\0', deps=0):
    """A PACx 403 archive: blocks are (name, PACx 402 bytes); with none, the root holds the resources (the types string stands in for its trees). deps: a dependency table's length"""
    data, entries = bytearray(), []
    for name, b in blocks:
        ch = lz4_chunks(b, stored={1})
        entries.append((name, sum(c for _, c, _ in ch), len(b), ch))
        data += b''.join(c for c, _, _ in ch)
    trees = bytearray(b'type tree\0' + types)
    trees += bytes(-len(trees) % 8)
    root = bytearray(b'PACx402L' + bytes(0x28)) + trees
    info_at = len(root)
    info = 0x10 + sum(0x20 + 8 * len(ch) for *_, ch in entries) if entries else 0
    tab = info_at + 0x10
    cks = tab + 0x20 * len(entries)
    names = cks + 8 * sum(len(ch) for *_, ch in entries)
    if entries: root += struct.pack('<QQ', len(entries), tab)
    off, k, nm = 0, cks, names
    for name, stored, ln, ch in entries:
        root += struct.pack('<QIIIIQ', nm, stored, ln, 0, len(ch), k)   # offset filled below
        k += 8 * len(ch); nm += len(name) + 1
    for *_, ch in entries:
        for _, c, u in ch: root += struct.pack('<II', c, u)
    for name, *_ in entries: root += name + b'\0'
    struct.pack_into('<II', root, 0x10, len(trees), info)
    ctab = (4 + 8 * -(-len(root) // 0x10000) + 7) & ~7   # the root's chunk count follows from its length
    head = bytearray(0x30 + deps + ctab)
    at = len(head)
    for i, (_, stored, *_) in enumerate(entries):   # the blocks follow the header, the root comes last
        struct.pack_into('<I', root, tab + 0x20 * i + 0x10, at); at += stored
    rch = lz4_chunks(bytes(root))
    head[:8] = b'PACx403L'
    struct.pack_into('<III', head, 0x10, at, sum(c for _, c, _ in rch), len(root))
    struct.pack_into('<II', head, 0x20, deps, ctab)
    struct.pack_into('<I', head, 0x30 + deps, len(rch))
    for i, (_, c, u) in enumerate(rch): struct.pack_into('<II', head, 0x34 + deps + 8 * i, c, u)
    return bytes(head + data + b''.join(c for c, _, _ in rch))


class NeedleTests(unittest.TestCase):
    def test_pacx403_invalid_lz4_chunk(self):
        d = bytearray(pacx403([]))
        root, stored = struct.unpack_from('<II', d, 0x10)
        d[root:root + stored] = bytes(stored)
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] corrupt\.pac: PACx 403 root: invalid LZ4 chunk'):
            split.pacx4_parts(d, 'corrupt.pac')

    def test_pacx403_truncated_header_and_root_chunk_table(self):
        deps = 0x18
        d = pacx403([], deps=deps)
        for length, section in ((0x24, 'header'), (0x30 + deps + 3, 'root chunk table'), (0x34 + deps + 7, 'root chunk table')):
            with self.subTest(length=length), self.assertRaisesRegex(SystemExit, f'PACx 403 {section}: .*lie outside'):
                split.scan_blob(d[:length], 'cut.pac', [])

    def test_pacx403_truncated_shader_root_header(self):
        root = b'PACx402LShader'
        d = bytearray(0x3c)
        d[:8] = b'PACx403L'
        struct.pack_into('<III4xII', d, 0x10, len(d), len(root), len(root), 0, 12)
        struct.pack_into('<III', d, 0x30, 1, len(root), len(root))
        with self.assertRaisesRegex(SystemExit, 'PACx 402 root header: .*lie outside'):
            split.scan_blob(d + root, 'cut.pac', [])

    def test_embedded_variants_and_headerless_compute(self):
        data = b'prefix\0' + container('vs', 'ps', 'cs') + b'suffix'
        found = []
        split.scan_blob(data, 'main', found)
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        for (_, stage, ctl, code) in found:
            self.assertEqual((ctl, code), shader(stage)[1:])
        keys = split.uniq_keys(found)
        # These stages are independent programs; the main/GSC wrapper is not a pair.
        self.assertEqual(split.var_pairs(found), set())
        self.assertEqual(len(split.separable_pairs(keys)[0]), 1)
        self.assertTrue(all('#NEEDLE@' in label for label, *_ in found))

    def test_container_bounds_reject_truncated_or_string_magic(self):
        self.assertEqual(split.needle_shaders(b'HNSHV002', 'short'), [])
        self.assertEqual(split.needle_shaders(container('vs')[:-1], 'truncated'), [])
        data = bytearray(container('vs'))
        struct.pack_into('>I', data, 8, 32)
        self.assertEqual(split.needle_shaders(data, 'short envelope'), [])

    def test_descriptor_cannot_borrow_code_from_next_record(self):
        data = bytearray(container('vs'))
        c = data.index(split.CTL_MAGIC)
        struct.pack_into('<I', data, c - 0x90 + 0x34, len(data))
        with self.assertRaisesRegex(SystemExit, 'outside its HNSHV002 container'):
            split.needle_shaders(data + container('ps'), 'bad code offset')

    def test_control_length_must_match_descriptor(self):
        data = bytearray(container('vs'))
        c = data.index(split.CTL_MAGIC)
        struct.pack_into('<I', data, c - 0x90 + 0x3c, 0x880)
        with self.assertRaisesRegex(SystemExit, 'control length'):
            split.needle_shaders(data, 'bad control length')


if __name__ == '__main__':
    unittest.main()
