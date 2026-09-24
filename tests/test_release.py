import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('release', Path(__file__).resolve().parents[1] / 'tests/package_release.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)

class ReleaseTests(unittest.TestCase):
    def test_release_version_matches_tag(self):
        self.assertEqual(release.version('0.1.0-rc.1', 'v0.1.0-rc.1'), 'v0.1.0-rc.1')

    def test_tag_mismatch_rejected(self):
        with self.assertRaises(ValueError):
            release.version('0.1.0-rc.1', 'v0.1.0')

    def test_invalid_version_rejected(self):
        for value in ('latest', '../test', '1.2', '1.2.3;echo bad', '01.2.3'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                release.version(value, '')
