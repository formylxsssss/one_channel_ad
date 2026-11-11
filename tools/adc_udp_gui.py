"""Run the existing acquisition GUI with only its built-in device transport replaced."""
import argparse
import importlib.util
import ipaddress
import json
import os
import sys
from pathlib import Path

from adc_udp_bridge import UdpBridgeServerThread


def binding_settings_path():
    return Path(os.environ.get('APPDATA', str(Path.home()))) / 'Channel1Acquisition' / 'udp_gui.json'


def saved_bind_ip():
    try:
        value = json.loads(binding_settings_path().read_text(encoding='utf-8'))['bind_ip']
        return str(ipaddress.IPv4Address(value))
    except (OSError, ValueError, KeyError, TypeError):
        return None


def remember_bind_ip(value):
    path = binding_settings_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({'bind_ip': str(ipaddress.IPv4Address(value))}), encoding='utf-8')


def gui_script_path(override=None):
    if override is not None:
        return Path(override)
    configured = os.environ.get('ADC_GUI_SCRIPT')
    return Path(configured) if configured else Path(__file__).with_name('adc_tcp_gui_5g_active_auto_bridge.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gui-script', type=Path)
    parser.add_argument('--device-id', type=int, choices=range(256), metavar='0..255')
    parser.add_argument('--device-port', type=int, default=9000)
    parser.add_argument('--gui-port', type=int, default=9001)
    parser.add_argument('--bind-ip', help='Local UDP bind address; use the board server IP when it is a local alias')
    parser.add_argument('--connect-on-start', action='store_true', help='Connect the GUI to its local Bridge after startup')
    args = parser.parse_args()
    explicit_bind = args.bind_ip is not None
    if explicit_bind:
        try:
            args.bind_ip = str(ipaddress.IPv4Address(args.bind_ip))
        except ValueError:
            parser.error('--bind-ip must be a local IPv4 address')
    else:
        args.bind_ip = saved_bind_ip()
    if not (1 <= args.device_port <= 65535 and 1 <= args.gui_port <= 65535):
        parser.error('Ports must be in 1..65535')
    if getattr(sys, 'frozen', False) and args.gui_script is None:
        import adc_tcp_gui_5g_active_auto_bridge as gui
    else:
        script = gui_script_path(args.gui_script).resolve()
        if not script.is_file():
            parser.error('GUI source is missing: ' + str(script))
        spec = importlib.util.spec_from_file_location('existing_adc_gui', script)
        gui = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gui)

    def bridge_factory(*positional, **keywords):
        return UdpBridgeServerThread(*positional, **keywords, device_id=args.device_id,
                                    bind_ip=args.bind_ip)

    def check_bridge(self):
        if self.bridge.started_event.is_set():
            self.append_log(f'[UDP] Bridge ready: device UDP {self.bridge.bind_ip}:{args.device_port}, GUI TCP {args.gui_port}')
            if explicit_bind:
                try:
                    remember_bind_ip(args.bind_ip)
                except OSError as error:
                    self.append_log(f'[UDP] Cannot remember binding address: {error}')
        elif self.bridge.failed_event.is_set():
            self.append_log('[UDP] Bridge failed: ' + self.bridge.failed_reason)
        else:
            self.root.after(500, lambda: check_bridge(self))

    original = gui.AdcGuiApp.__init__

    def initialize(self, root):
        original(self, root)
        root.title(root.title() + ' [UDP device transport]')
        if args.connect_on_start:
            def connect_when_ready():
                if self.bridge.started_event.is_set():
                    self.connect()
                elif not self.bridge.failed_event.is_set():
                    root.after(100, connect_when_ready)
            root.after(200, connect_when_ready)

    gui.BridgeServerThread = bridge_factory
    gui.DEFAULT_DEVICE_SERVER_PORT = args.device_port
    gui.DEFAULT_PORT = args.gui_port
    gui.AdcGuiApp._check_bridge_ports_once = check_bridge
    gui.AdcGuiApp.__init__ = initialize
    gui.main()


if __name__ == '__main__':
    main()
