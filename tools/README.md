# UDP 上位机

本目录包含完整上位机源码，可以独立复制到其他电脑运行或打包，
不再依赖 `D:\pycharm\one_channel_tcp` 中的文件。

## 文件

| 文件 | 用途 |
| --- | --- |
| adc_udp_gui.py | UDP 上位机启动入口、监听地址和本机 GUI 连接配置 |
| adc_udp_bridge.py | 设备 UDP 注册、收发、保活和有限窗口重排 |
| adc_tcp_gui_5g_active_auto_bridge.py | 复用的界面、绘图、参数操作和文件保存实现，旧文件名保留；由 UDP 入口替换通信层 |
| adc_udp_gui.spec | PyInstaller 单文件打包配置 |
| requirements.txt | 已测试的依赖版本，测试环境为 Windows Python 3.13.5 |
| 采集器上位机_UDP.exe | 打包产物，可直接运行，Git 忽略该二进制文件 |

应运行 `adc_udp_gui.py`，不要直接运行保留旧名称的界面模块，否则会启动原 TCP Bridge。

## 源码运行

在本目录执行以下 PowerShell 命令。Python 需包含 tkinter，Windows 官方完整安装版通常自带。

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe adc_udp_gui.py
```

用户界面连接 `127.0.0.1:9001`，该连接只用于同机 GUI 与 Bridge。
设备与服务器之间全部是 UDP，默认设备目的端口为 UDP 9000。

当前电脑配置了本地别名 `49.239.193.149`，联调可运行：

```powershell
.\.venv\Scripts\python.exe adc_udp_gui.py --bind-ip 49.239.193.149 --device-id 3 --connect-on-start
```

或运行同参数 EXE。成功显式绑定后会记住 IP，保存到
`%APPDATA%\Channel1Acquisition\udp_gui.json`；更换电脑/网络应重新指定实际本地 IP。
恢复通配绑定可传 `--bind-ip 0.0.0.0`，但多地址网卡仍需确保回包源 IP 正确。
NAT 服务器绑定自身真实本地地址，不能绑定未分配给本机的公网地址。
同时运行多个实例应使用不同端口，避免与旧上位机争用 UDP 9000/TCP 9001。

## 打包到本目录

先退出本目录下正在运行的同名 EXE，避免文件占用，然后执行：

```powershell
.\.venv\Scripts\python.exe -m PyInstaller --noconfirm --distpath . --workpath ../build/udp-gui-tools adc_udp_gui.spec
```

输出为本目录的 `采集器上位机_UDP.exe`，已包含界面模块，不需要另带 Python 文件或 Python 环境。
默认读取同目录界面源码；`--gui-script` 或环境变量 `ADC_GUI_SCRIPT` 仅用于有意替换界面实现，
正常使用无需设置。运行现有冻结 EXE 时，如需替换内置界面应重新打包。

该版本不增加丢包重传或逐包确认。多设备、字段定义及迁移约束见项目的
`docs/udp_host_integration_guide.md`。
