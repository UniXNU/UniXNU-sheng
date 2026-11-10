#!/usr/bin/env python3
"""Exercise firmware recovery without downloading or compiling target code."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('extract_files', Path(__file__).with_name('extract-files.py'))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
from firmware_cache import retained_inputs


class FirmwarePreparationTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.data = b'pinned keyboard firmware'
        self.digest = hashlib.sha256(self.data).hexdigest()
        self.target = self.root / '.cache/firmware/nanosic/MCU_Upgrade.bin'
        (self.root / 'proprietary-files.txt').write_text(
            '@fetch https://raw.githubusercontent.com/org/fw/commit/\n# Keyboard\n'
            f'nanosic/MCU_Upgrade.bin|{self.digest}\n')

    def run_extract(self, *args):
        with patch.object(sys, 'argv', ['extract-files.py', '--device', str(self.root), *args]), contextlib.redirect_stdout(io.StringIO()):
            return extract.main()

    def test_cached_file_skips_network_and_keeps_inode(self):
        extract.install(self.target, self.data)
        before = self.target.stat()
        with patch.object(extract, 'read_source', side_effect=AssertionError('network')):
            self.assertEqual(self.run_extract(), 0)
        self.assertEqual(self.target.stat(), before)

    def test_verified_repository_file_seeds_cache_offline(self):
        local = self.root / 'firmware/nanosic/MCU_Upgrade.bin'
        local.parent.mkdir(parents=True)
        local.write_bytes(self.data)
        with patch.object(extract, 'read_source', side_effect=AssertionError('network')):
            self.assertEqual(self.run_extract(), 0)
        self.assertEqual(self.target.read_bytes(), self.data)

    def test_missing_input_is_fetched_and_checked(self):
        with patch.object(extract, 'read_source', return_value=self.data):
            self.assertEqual(self.run_extract(), 0)
        self.assertEqual(self.target.read_bytes(), self.data)

    def test_wrong_hash_does_not_replace_existing_file(self):
        extract.install(self.target, b'old firmware')
        with patch.object(extract, 'read_source', return_value=b'wrong firmware'):
            self.assertEqual(self.run_extract(), 1)
        self.assertEqual(self.target.read_bytes(), b'old firmware')

    def test_failed_download_keeps_old_file(self):
        extract.install(self.target, b'old firmware')
        with patch.object(extract, 'read_source', side_effect=RuntimeError('HTTP 403')):
            self.assertEqual(self.run_extract(), 1)
        self.assertEqual(self.target.read_bytes(), b'old firmware')

    def test_check_never_downloads(self):
        with patch.object(extract, 'read_source', side_effect=AssertionError('network')):
            self.assertEqual(self.run_extract('--check'), 1)

    def test_section_does_not_fetch_other_inputs(self):
        with patch.object(extract, 'read_source', side_effect=AssertionError('network')):
            self.assertEqual(self.run_extract('--section', 'Touch'), 0)

    def test_failed_atomic_replace_cleans_temporary(self):
        extract.install(self.target, self.data)
        with patch.object(Path, 'replace', side_effect=OSError('no space')):
            with self.assertRaises(OSError):
                extract.install(self.target, b'replacement')
        self.assertEqual(self.target.read_bytes(), self.data)
        self.assertEqual(list(self.target.parent.iterdir()), [self.target])

    def cache(self):
        image = {'url': 'https://vendor/image.tgz', 'sha256': 'a' * 64}
        directory = self.root / 'stock'
        (directory / 'porting-inputs').mkdir(parents=True)
        selected = []
        for name in ('uefi.elf', 'xbl.img'):
            (directory / 'porting-inputs' / name).write_bytes(self.data)
            selected.append({'file': name, 'bytes': len(self.data), 'sha256': self.digest})
        record = dict(source_url=image['url'], archive_sha256=image['sha256'],
                      gzip_integrity_verified=True, selected=selected, archive_deleted=True)
        (directory / 'porting-inputs.json').write_text(json.dumps(record))
        return directory, image

    def test_retained_files_reuse_without_archive_or_range_state(self):
        directory, image = self.cache()
        self.assertTrue(retained_inputs(directory, image))

    def test_missing_retained_file_refuses_cached_marker(self):
        directory, image = self.cache()
        (directory / 'porting-inputs/uefi.elf').unlink()
        self.assertFalse(retained_inputs(directory, image))

    def test_same_size_corruption_refuses_cached_marker(self):
        directory, image = self.cache()
        (directory / 'porting-inputs/uefi.elf').write_bytes(b'X' * len(self.data))
        self.assertFalse(retained_inputs(directory, image))

    def test_wrong_source_refuses_cache(self):
        directory, image = self.cache()
        self.assertFalse(retained_inputs(directory, dict(image, sha256='b' * 64)))

    def test_malformed_record_refuses_cache(self):
        directory, image = self.cache()
        for content in ('{', '[]', 'null'):
            (directory / 'porting-inputs.json').write_text(content)
            self.assertFalse(retained_inputs(directory, image))


if __name__ == '__main__':
    unittest.main()
