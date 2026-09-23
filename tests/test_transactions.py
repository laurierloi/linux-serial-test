import os
import pty
import select
import struct
import subprocess
import time
import unittest
import tempfile
import zlib
from pathlib import Path
from test_pty import BINARY


def frame(kind, role, seq=0, payload=b'', run=123):
    header = struct.pack('!4sBBHII', b'LST1', kind, role, len(payload), run, seq)
    return header + payload + struct.pack('!I', zlib.crc32(header + payload))


class Transactions(unittest.TestCase):
    def start(self, role='responder', extra=()):
        m, s = pty.openpty()
        p = subprocess.Popen([BINARY, '-p', os.ttyname(s), '-m', '-n', '--transaction-role', role,
                              '--run-id', '123', '--transactions', '2', '--payload-bytes', '16',
                              '--transaction-timeout', '300', *extra], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.close, p, m, s)
        time.sleep(.08)
        return p, m

    @staticmethod
    def close(p, m, s):
        if p.poll() is None: p.kill()
        p.communicate()
        os.close(m); os.close(s)

    def test_partial_frame_never_replied_to(self):
        p, m = self.start()
        os.write(m, frame(1, 0)[:5])
        self.assertFalse(select.select([m], [], [], .1)[0])
        out, _ = p.communicate(timeout=2)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b'"error":"truncated_frame"', out)

    def test_crc_rejected(self):
        p, m = self.start()
        bad = bytearray(frame(1, 0)); bad[-1] ^= 1
        os.write(m, bad)
        out, _ = p.communicate(timeout=2)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b'"error":"crc"', out)

    def test_wrong_peer(self):
        p, m = self.start()
        os.write(m, frame(1, 0, run=124))
        out, _ = p.communicate(timeout=2)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b'"error":"run_id"', out)

    def test_local_echo(self):
        p, m = self.start()
        os.write(m, frame(1, 1))
        out, _ = p.communicate(timeout=2)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b'"error":"local_echo"', out)

    def test_oversized_frame_rejected_before_body(self):
        p,m=self.start()
        os.write(m,struct.pack('!4sBBHII',b'LST1',1,0,4097,123,0))
        out,_=p.communicate(timeout=2)
        self.assertNotEqual(p.returncode,0)
        self.assertIn(b'"error":"length"',out)

    def test_no_peer_timeout(self):
        p,_=self.start()
        out,_=p.communicate(timeout=2)
        self.assertNotEqual(p.returncode,0)
        self.assertIn(b'"error":"timeout"',out)

    def test_partial_request_never_replied_to(self):
        p,m=self.start()
        os.write(m,frame(1,0,payload=struct.pack('!II',2,16)))
        self.assertTrue(select.select([m],[],[],.2)[0]); os.read(m,4096)
        os.write(m,frame(3,0,seq=1,payload=bytes(16))[:21])
        self.assertFalse(select.select([m],[],[],.1)[0])
        out,_=p.communicate(timeout=2)
        self.assertIn(b'"error":"truncated_frame"',out)
        self.assertNotEqual(p.returncode,0)

    def test_fragmented_handshake(self):
        p,m=self.start()
        for byte in frame(1,0,payload=struct.pack('!II',2,16)):
            os.write(m,bytes([byte])); time.sleep(.001)
        self.assertTrue(select.select([m],[],[],.2)[0])
        reply=os.read(m,4096)
        self.assertEqual(reply,frame(2,1,payload=struct.pack('!II',2,16)))

    def test_wrong_sequence(self):
        p, m = self.start()
        os.write(m, frame(1, 0, payload=struct.pack('!II',2,16)))
        self.assertTrue(select.select([m], [], [], .2)[0])
        os.read(m,4096)
        os.write(m, frame(3, 0, seq=2, payload=bytes(16)))
        out, _ = p.communicate(timeout=2)
        self.assertNotEqual(p.returncode,0)
        self.assertIn(b'"error":"sequence"',out)

    def test_ready_marker_owned_and_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            marker=str(Path(directory)/'ready')
            p, _ = self.start(extra=('--ready-file',marker))
            self.assertTrue(Path(marker).exists())
            p.terminate(); p.communicate(timeout=2)
            self.assertFalse(Path(marker).exists())

    def test_existing_ready_marker_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            marker=Path(directory)/'ready'; marker.write_text('existing')
            p, _ = self.start(extra=('--ready-file',str(marker)))
            p.communicate(timeout=2)
            self.assertNotEqual(p.returncode,0)
            self.assertEqual(marker.read_text(),'existing')

    def test_strict_drain_requires_driver_support(self):
        p, _ = self.start(extra=('--strict-drain',))
        p.communicate(timeout=2)
        self.assertNotEqual(p.returncode,0)

    def test_pair_exact_exchange(self):
        a, ma = self.start('initiator', ('--transaction-timeout', '1000'))
        b, mb = self.start('responder', ('--transaction-timeout', '1000'))
        end = time.monotonic() + 4
        while time.monotonic() < end and (a.poll() is None or b.poll() is None):
            for fd in select.select([ma, mb], [], [], .02)[0]:
                data = os.read(fd, 8192)
                if data: os.write(mb if fd == ma else ma, data)
        for p in (a,b):
            out, err = p.communicate(timeout=.5)
            self.assertEqual(p.returncode, 0, (out,err))
            self.assertIn(b'"completed":2',out)
            self.assertIn(b'"payload_rx":32',out)
            self.assertIn(b'"payload_tx":32',out)

if __name__ == '__main__': unittest.main()
