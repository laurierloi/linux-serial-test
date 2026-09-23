import subprocess
import tempfile
import unittest
from pathlib import Path

class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = str(Path(cls.tmp.name) / 'driver')
        subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).with_name('driver.c')),'-lutil','-o',cls.binary],check=True)
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def test_driver_contracts(self):
        for case in ['short-write','credit','drain','read-error','preserve','reconfigure','reject','readback']:
            with self.subTest(case=case):
                result = subprocess.run([self.binary,case],capture_output=True,timeout=2)
                self.assertEqual(result.returncode, 2 if case in ('reject','readback') else 0, result.stderr.decode())
