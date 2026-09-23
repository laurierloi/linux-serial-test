import fcntl
import select
import termios
import tty
import os
import pty
import subprocess
import time
import unittest
from pathlib import Path

BINARY = os.environ.get('SERIAL_TEST_BINARY', str(Path(__file__).resolve().parents[1] / 'linux-serial-test'))

class SerialTests(unittest.TestCase):
    def run_rx(self, data=b'', extra=(), disconnect=False, terminate=False):
        master, slave = pty.openpty()
        command = [BINARY, '-p', os.ttyname(slave), '-b', '9600', '-m', '-n', '-t', '-i', '1', '--rx-bytes-threash', '16', *extra]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            time.sleep(.12)
            if data:
                os.write(master, data)
            if disconnect:
                os.close(master)
                master = -1
            if terminate:
                process.terminate()
            out, err = process.communicate(timeout=2.5)
            return process.returncode, out.decode(), err.decode()
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            if master >= 0:
                os.close(master)
            os.close(slave)

    def test_partial_response_finishes(self):
        rc, out, _ = self.run_rx(bytes(range(15)))
        self.assertEqual(rc, 0)
        self.assertIn('rx=15,', out)

    def test_partial_response_terminates(self):
        rc, out, _ = self.run_rx(bytes(range(15)), terminate=True)
        self.assertNotEqual(rc, 0)
        self.assertIn('rx=', out)

    def test_complete_response(self):
        rc, out, _ = self.run_rx(bytes(range(16)))
        self.assertEqual(rc, 0)
        self.assertIn('rx=16,', out)

    def test_disconnect_fails(self):
        rc, _, _ = self.run_rx(disconnect=True)
        self.assertNotEqual(rc, 0)

    def test_empty_receive_fails(self):
        rc, _, _ = self.run_rx()
        self.assertNotEqual(rc, 0)

    def test_exact_count_detects_whole_pattern_loss(self):
        rc, _, _ = self.run_rx(bytes(range(32)), ('--expected-rx', '288'))
        self.assertNotEqual(rc, 0)

    def test_exact_count_passes(self):
        rc, _, _ = self.run_rx(bytes(range(32)), ('--expected-rx', '32'))
        self.assertEqual(rc, 0)

    def test_short_threshold_option(self):
        rc, out, err = self.run_rx(bytes(range(16)), ('-M', '16'))
        self.assertEqual(rc, 0, err)
        self.assertNotIn('invalid option', err)

    def test_failed_lock_preserves_queued_input(self):
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            fcntl.flock(slave, fcntl.LOCK_EX | fcntl.LOCK_NB)
            os.write(master, b'pending')
            time.sleep(.05)
            proc = subprocess.run([BINARY, '-p', os.ttyname(slave), '-m', '-n', '-t', '-i', '1'], capture_output=True, timeout=2)
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(select.select([slave], [], [], .2)[0])
            self.assertEqual(os.read(slave, 7), b'pending')
        finally:
            os.close(master); os.close(slave)

    def test_termios_restored(self):
        master, slave = pty.openpty()
        previous = termios.tcgetattr(slave)
        try:
            subprocess.run([BINARY, '-p', os.ttyname(slave), '-m', '-n', '-t', '-i', '1'], capture_output=True, timeout=2)
            self.assertEqual(termios.tcgetattr(slave), previous)
        finally:
            os.close(master); os.close(slave)

    def test_write_follow_exchange(self):
        master, slave = pty.openpty()
        proc = subprocess.Popen([BINARY, '-p', os.ttyname(slave), '-m', '-n', '-K', '-F', '-w', '16', '-M', '16', '-o', '1', '-i', '2'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        exchanged = 0
        deadline = time.monotonic() + 3
        try:
            while proc.poll() is None and time.monotonic() < deadline:
                if select.select([master], [], [], .02)[0]:
                    data = os.read(master, 1024)
                    exchanged += len(data)
                    os.write(master, data)
            out, err = proc.communicate(timeout=.5)
            self.assertEqual(proc.returncode, 0, (out, err))
            self.assertGreater(exchanged, 16)
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate()
            os.close(master); os.close(slave)

    def test_invalid_options_fail(self):
        for args in [('--typo',), ('--tx-bytes', '-1'), ('--rx-bytes-threash', '-1'), ('--parity', 'bogus'), ('--rs485', '1.invalid'), ('--baud', 'abc')]:
            with self.subTest(args=args):
                proc = subprocess.run([BINARY, *args], capture_output=True, timeout=2)
                self.assertNotEqual(proc.returncode, 0)

if __name__ == '__main__':
    unittest.main()
