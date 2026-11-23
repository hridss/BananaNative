"""uepak.py's Zstandard pak blocks (from upstream nxnative 22222648b): a frame is decoded to exactly its block's raw size, with or without a content size in its header;
a wrong size, a truncated frame or trailing bytes are refused. Synthetic data only; needs Python 3.14's compression.zstd or backports.zstd."""
from pathlib import Path
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak

try:
    zstd = uepak.zstd_mod()
except ImportError:
    zstd = None


@unittest.skipIf(zstd is None, 'no compression.zstd or backports.zstd')
class ZstdBlocks(unittest.TestCase):
    RAW = bytes(range(256)) * 40

    def frame(self, sized=True):
        if sized:
            return zstd.compress(self.RAW)
        c = zstd.ZstdCompressor()   # streamed: the frame header has no content size
        return c.compress(self.RAW) + c.flush()

    def test_exact_size_with_and_without_content_size(self):
        for sized in (True, False):
            self.assertEqual(uepak.zstd_block(self.frame(sized), len(self.RAW)), self.RAW)

    def test_refused(self):
        f = self.frame()
        for bad, k in ((f, len(self.RAW) - 1), (f, len(self.RAW) + 1), (f, -1), (f[:-4], len(self.RAW)), (f + b'\0', len(self.RAW))):
            with self.assertRaises(zstd.ZstdError):
                uepak.zstd_block(bad, k)

    def test_empty_frame(self):
        self.assertEqual(uepak.zstd_block(zstd.compress(b''), 0), b'')


if __name__ == '__main__':
    unittest.main()
