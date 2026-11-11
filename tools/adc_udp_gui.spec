# -*- mode: python ; coding: utf-8 -*-
import os
from pathlib import Path

gui_script = Path(os.environ.get('ADC_GUI_SCRIPT',
    'D:/pycharm/one_channel_tcp/adc_tcp_gui_5g_active_auto_bridge.py'))
if not gui_script.is_file():
    raise SystemExit('Set ADC_GUI_SCRIPT to the original acquisition GUI script')

a = Analysis(
    [str(Path(SPECPATH) / 'adc_udp_gui.py')],
    pathex=[str(gui_script.parent), SPECPATH],
    binaries=[], datas=[],
    hiddenimports=['adc_tcp_gui_5g_active_auto_bridge'],
    hookspath=[], hooksconfig={'matplotlib': {'backends': ['TkAgg']}},
    runtime_hooks=[], excludes=[], noarchive=False, optimize=0,
)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, a.binaries, a.datas, [],
    name='采集器上位机_UDP', debug=False, strip=False, upx=False,
    console=False, disable_windowed_traceback=False)
