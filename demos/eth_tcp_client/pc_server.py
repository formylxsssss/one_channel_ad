"""TCP peer for the Ethernet-only board demo; standard library only."""

import argparse
import socketserver
import threading
import time


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        print(f"CONNECTED {self.client_address}", flush=True)
        self.connection.settimeout(10)
        try:
            while True:
                line = self.rfile.readline(128)
                if not line:
                    break
                print(f"RX {self.client_address}: {line!r}", flush=True)
                if line == b"BOARD_PING\n":
                    self.wfile.write(b"PC_PONG\n")
                    self.wfile.flush()
                    print(f"TX {self.client_address}: PC_PONG", flush=True)
        except (TimeoutError, OSError) as exc:
            print(f"CLIENT END {self.client_address}: {exc}", flush=True)
        finally:
            print(f"DISCONNECTED {self.client_address}", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.11.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--duration", type=int, default=0,
                        help="Stop after this many seconds; 0 keeps listening.")
    args = parser.parse_args()
    with Server((args.host, args.port), Handler) as server:
        print(f"LISTENING {args.host}:{args.port}", flush=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            if args.duration:
                time.sleep(args.duration)
            else:
                while True:
                    time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            server.shutdown()


if __name__ == "__main__":
    main()
