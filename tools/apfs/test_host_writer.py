"""Portable host regressions; no firmware, Apple image or phone required."""
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

import apfs
import mkapfs


class HostWriterTests(unittest.TestCase):
    def test_crlf_manifest_link_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / 'manifest'
            manifest.write_bytes(b'120777\t0\t0\talias\t/private/var\r\n')
            root = mkapfs.Node('', stat.S_IFDIR | 0o755)
            mkapfs.apply_manifest(root, manifest)
            self.assertEqual(root.children['alias'].target, b'/private/var')

    def test_windows_binary_positioned_reads(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'raw'
            data = b'A\r\nB\x1aC' + bytes(range(256))
            path.write_bytes(data)
            image = apfs.RawImage(path)
            try:
                self.assertEqual(image.read(1, 5), data[1:6])
                self.assertEqual(image.read(0, len(data)), data)
                self.assertEqual(image.read(256, 6), data[256:262])
            finally:
                image.close()


if __name__ == '__main__':
    unittest.main()
