"""Execute the production UDP transport as Cortex-M4 code with mocked LwIP/HAL."""
import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from adc_udp_bridge import make_frame, valid_frame
from test_adc_dc_remove import Stream


class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stream = Stream(False,
            extra_sources=('device/adc_udp_protocol.c', 'device/app_net_config.c',
                           'tests/udp_stubs/runtime.c'),
            extra_includes=('-Itests/udp_stubs', '-DADC_TCP_TRACE_ENABLE=0',
                            '-DADC_STREAM_STAT_LOG_ENABLE=0', '--specs=nosys.specs'))

    def setUp(self):
        self.s = self.stream
        self.s.call('TestReset')
        self.task(2)
        self.assertEqual([x[3] for x in self.packets()], [0x30, 0x10])
        self.send(2)
        self.task()
        self.s.call('TestClearSent')

    def task(self, n=1):
        for _ in range(n):
            self.s.call('AdcTcpServer_Task')

    def raw(self, frame, wrong=False):
        self.s.uc.mem_write(0x20030000, frame)
        self.s.call('TestWrongPeer' if wrong else 'TestReceive', 0x20030000, len(frame))

    def send(self, kind, payload=b''):
        self.raw(make_frame(kind, payload, 42))

    def packets(self):
        result = []
        for i in range(self.s.call('TestSentCount')):
            length = self.s.call('TestCopySent', i, 0x20031000)
            data = bytes(self.s.uc.mem_read(0x20031000, length))
            self.assertTrue(valid_frame(data), data.hex())
            result.append(data)
        return result

    def start(self):
        self.send(3)
        self.task()
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 1)
        self.s.call('TestClearSent')

    def test_ack_is_sent_before_start_and_transient_failure_retries(self):
        self.s.call('TestFailSend', 1)
        self.send(3)
        self.task()
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 0)
        self.assertEqual(self.packets(), [])
        self.task()
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 1)
        self.assertEqual([x[3] for x in self.packets()], [0x10])

    def test_split_frames_preserve_all_sample_bytes_and_sequence(self):
        for bits in (16, 24):
            with self.subTest(bits=bits):
                self.setUp()
                self.send(1, struct.pack('<IBBBB', 50000, bits, 2, 0, 0))
                self.task()
                self.assertEqual(self.s.call('TestFlashSaves'), 1)
                self.s.bits = bits
                self.start()
                samples = [((i * 123) % 60000) - 30000 for i in range(790)]
                self.s.feed(samples, drain=False)
                self.s.call('TestFailSend', 1)
                self.task()
                self.assertEqual(self.packets(), [])
                self.assertEqual(self.s.call('AdcStream_GetReadyCount'), 1)
                self.task()
                frames = self.packets()
                self.assertEqual(len(frames), 2)
                offset = 0
                raw = bytearray()
                for frame in frames:
                    self.assertLessEqual(len(frame) + 28, 1280)
                    fs, width, gain, bps, reserved, first, count, size = struct.unpack_from('<IBBBBIHH', frame, 10)
                    self.assertEqual((fs, width, gain, bps, reserved), (48828, bits, 2, bits // 8, 0))
                    self.assertEqual(first, offset)
                    self.assertEqual(size, count * bps)
                    offset += count
                    raw.extend(frame[26:-2])
                self.assertEqual(offset, len(samples))
                expected = b''.join((x & ((1 << bits) - 1)).to_bytes(bits // 8, 'big') for x in samples)
                self.assertEqual(raw, expected)
                self.assertEqual(self.s.call('AdcStream_GetReadyCount'), 0)
                self.assertEqual(self.s.call('AdcStream_GetOverrunCount'), 0)

    def test_stop_drains_partial_data_before_ack(self):
        self.start()
        self.s.bits = 24
        self.s.feed([1, -2, 3, -4, 5, -6, 7], drain=False)
        self.send(4)
        self.task(2)
        frames = self.packets()
        self.assertEqual([x[3] for x in frames], [0x81, 0x10])
        self.assertEqual(int.from_bytes(frames[0][22:24], 'little'), 7)
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 0)

    def test_sample_sequence_wrap_does_not_stop_stream_or_corrupt_subframes(self):
        self.start()
        self.s.bits = 24
        self.s.uc.mem_write(self.s.symbols['s_sample_seq'], struct.pack('<I', 0xFFFFFFF0))
        self.s.feed([123] * 790, drain=False)
        self.task()
        frames = self.packets()
        self.assertEqual(len(frames), 2)
        self.assertEqual(int.from_bytes(frames[0][18:22], 'little'), 0xFFFFFFF0)
        self.assertEqual(int.from_bytes(frames[1][18:22], 'little'), (0xFFFFFFF0 + 408) & 0xFFFFFFFF)
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 1)
        self.assertEqual(self.s.call('AdcStream_GetOverrunCount'), 0)

    def test_timeout_and_link_loss_require_real_peer_before_auto_resume(self):
        self.start()
        self.s.call('TestTick', 30001)
        self.task()
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 0)
        self.assertEqual(self.s.call('TestConnected'), 0)
        self.s.call('TestTick', 33002)
        self.task(2)
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 0)
        self.send(2)
        self.task()
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 1)
        self.s.call('TestLink', 0)
        self.task()
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 0)
        self.s.call('TestLink', 1)
        self.s.call('TestTick', 36003)
        self.task(2)
        self.send(4)
        self.task(2)
        self.assertEqual(self.s.call('AdcStream_IsRunning'), 0)

    def test_invalid_crc_length_version_and_peer_do_not_keep_session_alive(self):
        for mutate in ('crc', 'length', 'version', 'peer'):
            with self.subTest(mutate=mutate):
                self.setUp()
                frame = bytearray(make_frame(2))
                if mutate == 'crc': frame[-1] ^= 1
                if mutate == 'length': frame[4] = 255
                if mutate == 'version': frame[2] = 2
                self.s.call('TestTick', 29999)
                self.raw(bytes(frame), wrong=mutate == 'peer')
                self.task()
                self.s.call('TestTick', 30001)
                self.task()
                self.assertEqual(self.s.call('TestConnected'), 0)

    def test_tick_wraparound(self):
        self.s.call('TestTick', 0xFFFFFF00)
        self.s.call('AdcTcpServer_Init')
        self.task(2)
        self.send(2)
        self.task()
        self.s.call('TestTick', 0x1000)
        self.task()
        self.assertEqual(self.s.call('TestConnected'), 1)
        self.s.call('TestTick', 0x8000)
        self.task()
        self.assertEqual(self.s.call('TestConnected'), 0)

    def test_network_commands_and_reconnect_preserve_payload_layout(self):
        self.send(0x21)
        self.task()
        frame = self.packets()[0]
        self.assertEqual((frame[3], len(frame)), (0x22, 48))
        self.s.call('TestClearSent')
        self.send(0x20, bytes([127, 0, 0, 1]) + struct.pack('<HBB', 9100, 9, 3))
        self.task()
        frame = self.packets()[0]
        self.assertEqual(frame[14:21], bytes([127, 0, 0, 1]) + struct.pack('<HB', 9100, 9))
        self.assertEqual(self.s.call('TestConnected'), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
