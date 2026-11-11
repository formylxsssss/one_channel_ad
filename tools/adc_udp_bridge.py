"""UDP device transport with an unchanged TCP byte-stream interface for the GUI."""
import argparse
import asyncio
import queue
import socket
import struct
import threading
import time

MAGIC = b'\x5a\xa5'
MAX_DATAGRAM = 1252
MAX_COMMAND = 256
REGISTER = 0x30
GET_PARAM = 0x02


def _crc_table():
    result = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
        result.append(value)
    return result


CRC_TABLE = _crc_table()


def crc16(data):
    result = 0xFFFF
    for value in data:
        result = (result >> 8) ^ CRC_TABLE[(result ^ value) & 255]
    return result


def make_frame(kind, payload=b'', sequence=0):
    body = struct.pack('<HBBHI', 0xA55A, 1, kind, len(payload), sequence) + payload
    return body + struct.pack('<H', crc16(body))


def valid_frame(data, limit=MAX_DATAGRAM):
    return (12 <= len(data) <= limit and data[:3] == MAGIC + b'\x01'
            and int.from_bytes(data[4:6], 'little') + 12 == len(data)
            and crc16(data[:-2]) == int.from_bytes(data[-2:], 'little'))


class FrameOrderer:
    """Bounded reordering, never inventing or retransmitting missing samples."""
    def __init__(self, emit, delay=0.02, capacity=64):
        self.emit = emit
        self.delay = delay
        self.capacity = capacity
        self.expected = None
        self.pending = {}
        self.gaps = 0
        self.duplicates = 0

    def feed(self, frame, now):
        sequence = int.from_bytes(frame[6:10], 'little')
        if self.expected is None:
            self.expected = sequence
        delta = (sequence - self.expected) & 0xFFFFFFFF
        if delta >= 0x80000000 or sequence in self.pending:
            self.duplicates += 1
            return
        self.pending[sequence] = (frame, now)
        self.flush(now, force=len(self.pending) >= self.capacity)

    def flush(self, now, force=False):
        while self.pending:
            if self.expected not in self.pending:
                oldest = min(item[1] for item in self.pending.values())
                if not force and now - oldest < self.delay:
                    return
                following = min(self.pending, key=lambda n: (n - self.expected) & 0xFFFFFFFF)
                self.gaps += (following - self.expected) & 0xFFFFFFFF
                self.expected = following
            frame, _ = self.pending.pop(self.expected)
            self.emit(frame)
            self.expected = (self.expected + 1) & 0xFFFFFFFF
            force = False


class _Datagrams(asyncio.DatagramProtocol):
    def __init__(self, owner):
        self.owner = owner

    def datagram_received(self, data, address):
        self.owner.receive(data, address)

    def error_received(self, error):
        self.owner._post_log(f'UDP socket error: {error}')


class UdpBridgeServerThread(threading.Thread):
    def __init__(self, event_queue=None, host='0.0.0.0', device_port=9000,
                 gui_port=9001, device_id=None, bind_ip=None):
        super().__init__(daemon=True)
        self.event_queue = event_queue if event_queue is not None else queue.Queue()
        self.host, self.device_port, self.gui_port = host, device_port, gui_port
        self.bind_ip = bind_ip if bind_ip is not None else host
        self.device_id = device_id
        self.selected_id = device_id
        self.stop_event = threading.Event()
        self.started_event = threading.Event()
        self.stopped_event = threading.Event()
        self.failed_event = threading.Event()
        self.failed_reason = ''
        self.loop = None
        self.transport = None
        self.server = None
        self.clients = {}
        self.endpoint = None
        self.last_rx = 0.0
        self.last_probe = 0.0
        self.command_pause = 0.0
        self.probe_sequence = 0xF0000000
        self.registration = None
        self.orderer = FrameOrderer(self.broadcast)
        self.bad_frames = 0
        self.ignored_devices = set()

    def _post_log(self, message):
        self.event_queue.put(('log', f'[UDP BRIDGE] {message}'))

    def receive(self, data, address):
        if not valid_frame(data):
            self.bad_frames += 1
            return
        now = time.monotonic()
        if data[3] == REGISTER:
            if len(data) != 44:
                self.bad_frames += 1
                return
            device_id = data[10]
            if self.selected_id is None:
                self.selected_id = device_id
            if device_id != self.selected_id:
                if device_id not in self.ignored_devices:
                    self._post_log(f'Ignoring device {device_id}; this GUI is bound to ID {self.selected_id}')
                    self.ignored_devices.add(device_id)
                return
            sequence = int.from_bytes(data[6:10], 'little')
            reboot = sequence == 0 and self.orderer.expected not in (None, 0, 1)
            if address != self.endpoint or reboot:
                self.endpoint = address
                self.orderer = FrameOrderer(self.broadcast)
                self.last_probe = 0.0
                self._post_log(f'Device {device_id} registered from {address}')
            self.registration = data
        if address != self.endpoint:
            return
        self.last_rx = now
        self.orderer.feed(data, now)

    def broadcast(self, data):
        # Never block UDP reception behind a slow GUI or accumulate unbounded tasks.
        for writer, outbox in list(self.clients.items()):
            try:
                outbox.put_nowait(data)
            except asyncio.QueueFull:
                self._post_log('GUI cannot keep up; closing its connection instead of hiding dropped data')
                writer.close()
                self.clients.pop(writer, None)

    async def _write_gui(self, writer, outbox):
        while True:
            writer.write(await outbox.get())
            await writer.drain()

    async def handle_gui(self, reader, writer):
        outbox = asyncio.Queue(maxsize=128)
        self.clients[writer] = outbox
        sender = asyncio.create_task(self._write_gui(writer, outbox))
        pending = bytearray()
        if self.registration:
            outbox.put_nowait(self.registration)
        self._post_log('GUI connected')
        try:
            while not self.stop_event.is_set():
                chunk = await reader.read(4096)
                if not chunk:
                    break
                pending.extend(chunk)
                while len(pending) >= 12:
                    if pending[:2] != MAGIC:
                        del pending[0]
                        continue
                    size = int.from_bytes(pending[4:6], 'little') + 12
                    if size > MAX_COMMAND:
                        del pending[0]
                        continue
                    if len(pending) < size:
                        break
                    frame = bytes(pending[:size])
                    del pending[:size]
                    if not valid_frame(frame, MAX_COMMAND):
                        continue
                    if self.endpoint and time.monotonic() - self.last_rx < 30:
                        self.transport.sendto(frame, self.endpoint)
                        self.command_pause = time.monotonic() + 2
                    else:
                        self._post_log('No registered live device; command was not transmitted')
        except (ConnectionError, OSError):
            pass
        finally:
            self.clients.pop(writer, None)
            sender.cancel()
            await asyncio.gather(sender, return_exceptions=True)
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    async def _run_async(self):
        self.loop = asyncio.get_running_loop()
        try:
            self.transport, _ = await self.loop.create_datagram_endpoint(
                lambda: _Datagrams(self), local_addr=(self.bind_ip, self.device_port),
                family=socket.AF_INET)
            self.transport.get_extra_info('socket').setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            self.device_port = self.transport.get_extra_info('sockname')[1]
            self.server = await asyncio.start_server(self.handle_gui, self.host, self.gui_port)
            self.gui_port = self.server.sockets[0].getsockname()[1]
            self.started_event.set()
            self._post_log(f'Listening: device UDP {self.bind_ip}:{self.device_port}, GUI TCP {self.host}:{self.gui_port}')
            while not self.stop_event.is_set():
                now = time.monotonic()
                self.orderer.flush(now)
                if (self.endpoint and now - self.last_rx < 30 and now >= self.command_pause
                        and now - self.last_probe >= 5):
                    # Existing GET_PARAM command, no new wire-protocol heartbeat type.
                    self.transport.sendto(make_frame(GET_PARAM, sequence=self.probe_sequence), self.endpoint)
                    self.probe_sequence = (self.probe_sequence + 1) & 0xFFFFFFFF
                    self.last_probe = now
                await asyncio.sleep(0.01)
        except Exception as error:
            self.failed_reason = str(error)
            self.failed_event.set()
            self._post_log(f'Bridge failed: {error}')
        finally:
            if self.server:
                self.server.close()
                await self.server.wait_closed()
            for writer in list(self.clients):
                writer.close()
            self.clients.clear()
            if self.transport:
                self.transport.close()
            self.stopped_event.set()

    def run(self):
        asyncio.run(self._run_async())

    def stop(self):
        self.stop_event.set()
        if self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(lambda: None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--bind-ip', help='Bind device UDP replies to this local IPv4 address')
    parser.add_argument('--device-port', type=int, default=9000)
    parser.add_argument('--gui-port', type=int, default=9001)
    parser.add_argument('--device-id', type=int, choices=range(256), metavar='0..255')
    args = parser.parse_args()
    server = UdpBridgeServerThread(host=args.host, device_port=args.device_port,
                                   gui_port=args.gui_port, device_id=args.device_id,
                                   bind_ip=args.bind_ip)
    server.start()
    try:
        while server.is_alive():
            try:
                print(server.event_queue.get(timeout=0.5)[1], flush=True)
            except queue.Empty:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
        server.join(timeout=3)
    if server.failed_event.is_set():
        raise SystemExit(server.failed_reason)


if __name__ == '__main__':
    main()
