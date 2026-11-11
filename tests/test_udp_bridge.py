import asyncio
import socket
import tempfile
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from adc_udp_bridge import FrameOrderer, UdpBridgeServerThread, GET_PARAM, crc16, make_frame, valid_frame
from adc_udp_gui import binding_settings_path, saved_bind_ip, remember_bind_ip, gui_script_path


class OrderTests(unittest.TestCase):
    def test_gui_source_is_local_and_overrides_are_explicit(self):
        with patch.dict(os.environ, {}, clear=True):
            local = gui_script_path()
            self.assertEqual(local.parent, Path(__file__).resolve().parents[1] / 'tools')
            self.assertTrue(local.is_file())
        with patch.dict(os.environ, {'ADC_GUI_SCRIPT': 'custom/gui.py'}):
            self.assertEqual(gui_script_path(), Path('custom/gui.py'))
            self.assertEqual(gui_script_path('explicit.py'), Path('explicit.py'))

    def test_saved_local_binding_is_validated(self):
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, {'APPDATA': folder}):
            self.assertIsNone(saved_bind_ip())
            remember_bind_ip('127.0.0.2')
            self.assertEqual(saved_bind_ip(), '127.0.0.2')
            binding_settings_path().write_text('{broken', encoding='utf-8')
            self.assertIsNone(saved_bind_ip())
            binding_settings_path().write_text('{"bind_ip": "not-an-ip"}', encoding='utf-8')
            self.assertIsNone(saved_bind_ip())

    def test_crc_and_reorder_loss_duplicate_wrap(self):
        self.assertEqual(crc16(b'123456789'), 0x4B37)
        frames = []
        order = FrameOrderer(frames.append)
        for seq in (0xFFFFFFFE, 0, 0xFFFFFFFF, 0):
            order.feed(make_frame(0x81, sequence=seq), 1)
        self.assertEqual([int.from_bytes(f[6:10], 'little') for f in frames], [0xFFFFFFFE, 0xFFFFFFFF, 0])
        self.assertEqual(order.duplicates, 1)
        order.feed(make_frame(0x81, sequence=2), 1)
        self.assertEqual(len(frames), 3)
        order.flush(1.03)
        self.assertEqual(len(frames), 4)
        self.assertEqual(order.gaps, 1)

    def test_memory_is_bounded_with_large_sequence_gaps(self):
        order = FrameOrderer(lambda _: None, capacity=8)
        order.feed(make_frame(0x30), 0)
        for seq in range(2, 500, 2):
            order.feed(make_frame(0x81, sequence=seq), 0)
            self.assertLess(len(order.pending), 8)


class _Device(asyncio.DatagramProtocol):
    def __init__(self):
        self.received = asyncio.Queue()

    def datagram_received(self, data, address):
        self.received.put_nowait((data, address))


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.bridge = UdpBridgeServerThread(host='127.0.0.1', device_port=0, gui_port=0, device_id=3)
        self.bridge.start()
        self.assertTrue(self.bridge.started_event.wait(3), self.bridge.failed_reason)
        self.transport, self.device = await asyncio.get_running_loop().create_datagram_endpoint(
            _Device, local_addr=('127.0.0.1', 0))
        self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.bridge.gui_port)

    async def asyncTearDown(self):
        self.writer.close()
        await self.writer.wait_closed()
        self.transport.close()
        self.bridge.stop()
        self.bridge.join(timeout=3)
        self.assertFalse(self.bridge.is_alive())
        self.assertTrue(self.bridge.stopped_event.is_set())

    def register(self, device_id=3):
        self.transport.sendto(make_frame(0x30, bytes([device_id]) + bytes(31)),
                              ('127.0.0.1', self.bridge.device_port))

    async def frame(self):
        header = await asyncio.wait_for(self.reader.readexactly(10), 2)
        rest = await asyncio.wait_for(self.reader.readexactly(int.from_bytes(header[4:6], 'little') + 2), 2)
        self.assertTrue(valid_frame(header + rest))
        return header + rest

    async def test_registration_keepalive_command_and_reordered_data(self):
        self.register()
        self.assertEqual((await self.frame())[3], 0x30)
        probe, _ = await asyncio.wait_for(self.device.received.get(), 2)
        self.assertEqual(probe[3], 2)
        # TCP commands may be split across arbitrary GUI writes.
        command = make_frame(3, sequence=123)
        self.writer.write(command[:5])
        await self.writer.drain()
        self.writer.write(command[5:])
        await self.writer.drain()
        forwarded, _ = await asyncio.wait_for(self.device.received.get(), 2)
        self.assertEqual(forwarded, command)
        address = ('127.0.0.1', self.bridge.device_port)
        frames = [make_frame(0x10, bytes(44), sequence=1),
                  make_frame(0x81, bytes(16), sequence=2),
                  make_frame(0x81, bytes(16), sequence=3)]
        for f in (frames[0], frames[2], frames[1], frames[1]):
            self.transport.sendto(f, address)
        self.assertEqual([await self.frame() for _ in range(3)], frames)

    async def test_unselected_id_and_bad_crc_are_not_mixed_into_gui(self):
        self.register(4)
        await asyncio.sleep(0.04)
        self.assertIsNone(self.bridge.endpoint)
        bad = bytearray(make_frame(0x30, bytes([3]) + bytes(31)))
        bad[-1] ^= 1
        self.transport.sendto(bad, ('127.0.0.1', self.bridge.device_port))
        await asyncio.sleep(0.04)
        self.assertIsNone(self.bridge.endpoint)
        self.assertEqual(self.bridge.bad_frames, 1)
        self.register(3)
        self.assertEqual((await self.frame())[10], 3)


class BoundSourceTests(unittest.IsolatedAsyncioTestCase):
    async def test_connected_udp_peer_receives_reply_from_configured_alias(self):
        # A connected UDP peer rejects replies from another local IP, like LwIP.
        bridge = UdpBridgeServerThread(host='127.0.0.1', bind_ip='127.0.0.2',
                                       device_port=0, gui_port=0, device_id=3)
        bridge.start()
        device = None
        try:
            self.assertTrue(bridge.started_event.wait(3), bridge.failed_reason)
            device = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            device.bind(('127.0.0.1', 0))
            device.connect(('127.0.0.2', bridge.device_port))
            device.setblocking(False)
            loop = asyncio.get_running_loop()
            await loop.sock_sendall(device, make_frame(0x30, bytes([3]) + bytes(31)))
            response = await asyncio.wait_for(loop.sock_recv(device, 2048), 2)
            self.assertTrue(valid_frame(response))
            self.assertEqual(response[3], GET_PARAM)
        finally:
            if device:
                device.close()
            bridge.stop()
            bridge.join(timeout=3)
            self.assertFalse(bridge.is_alive())


if __name__ == '__main__':
    unittest.main(verbosity=2)
