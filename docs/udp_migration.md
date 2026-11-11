# UDP 分支使用说明

## 修改范围

工作目录：`E:\AMY_ALL_CODE\ad_one_channel`，分支：`udp_client`。

下位机的控制命令、回执、注册及采样数据全部改用 UDP，主动访问 Flash 中配置的
`server_ip:server_port`，默认仍是 `49.239.193.149:9000`。本机 UDP 源端口由 LwIP 分配。
为兼容 main.c 和构建配置，保留 `AdcTcpServer_Init/Task` 接口及源文件名；这些接口不再打开 TCP socket。

未修改 ADC 初始化、采样 DMA、采样档位、实际采样率计算、16/24 位编码、增益、100Hz 去直流、
Flash 记录版本或网络参数结构。不要为了升级 UDP 擦除参数扇区。

## 应用协议保持不变

帧仍为：10 字节帧头＋payload＋2 字节 CRC16/Modbus。
magic=0xA55A、version=1、字段大小端、帧序号、样点序号、DATA meta 均不变。

| 类型 | 功能 | 保留的行为 |
| --- | --- | --- |
| 0x01 | SET_PARAM | 采样率、位数、增益校验、配置 ADC 并保存 Flash |
| 0x02 | GET_PARAM | 原 44 字节 ACK payload |
| 0x03 | START | 先提交 START ACK，再开始采集 |
| 0x04 | STOP | 停采、提交剩余完整及部分采样块后，再提交 STOP ACK |
| 0x20 | SET_NET | 原 8 字节及 28 字节扩展格式，保留保存与重连标志 |
| 0x21 | GET_NET | 原 36 字节 NET_ACK payload |
| 0x30 | REGISTER | 原 32 字节 payload，含设备 ID |
| 0x81 | DATA | 原 16 字节 meta 和原始样本编码 |

一个 UDP 数据报只包含一个完整应用帧；不跨 UDP 数据报拼接命令。
DATA 的 sample_count/data_bytes 本来就是可变字段，本版利用它拆分内部 790 样点块。
默认 netif MTU=1280，每个 UDP payload 不超过 1252 字节，因此 IPv4 包不超过 1280 字节，
不依赖 IP 分片。24 位通常拆为 408＋382 样点，16 位拆为 612＋178 样点。
拆分会重新计算帧 CRC 和帧序号，并保持连续的 first_sample_seq；不会插值、补零或更改样本。
上位机必须按 DATA meta 的 sample_count、bytes_per_sample、fs_hz 解析，不能硬编码每包 790 点。

原 ACK 中 TCP 专属字段保留位置，但 sndbuf、unacked 和 TCP 已确认 DATA 帧数返回 0；
原发送内存错误字段承载 UDP 本地发送错误累计数。0 不代表网络确认了全部 UDP 数据。

## UDP 与 TCP 的必要差异

用户已选择全部 UDP，并接受丢包和乱序差异。本版没有实现可靠 UDP 或数据重传。
发送返回成功仅表示本地协议栈接收了数据，不保证远端收到。START ACK 与 DATA、STOP DATA 与
STOP ACK 的先后顺序仅在本地提交时保证；网络上仍可能丢失、重复或乱序。

原应用 ACK 没有请求 ID，不能无歧义匹配并发请求。因此兼容 Bridge 不盲目自动重发控制命令；
上位机应串行发命令，超时后查询状态再决定是否重试，尤其不要无限重复 SET_PARAM 写 Flash。
重复 START 在已运行时只回执，不重启；UDP 本身不提供 exactly-once 执行语义。
现有帧 CRC 用于检测错误，不是身份认证；只应在可信网络和受限制的服务端端口使用。

## 保活、断链和恢复

- 板子创建 UDP PCB 不代表远端在线，收到合法、来自已配置 IP/端口的命令后才标记在线。
- REGISTER 按 reconnect_ms 周期发送；配套 Bridge 使用现有 GET_PARAM 每 5 秒保活，不新增命令码。
- 无论正在采集还是空闲，30 秒未收到服务端合法命令会停采、清理会话，并按 reconnect_ms 重建注册。
- 网线或 netif 断开会立即停采并清理会话，链路恢复后重新注册。
- 中断前仍请求采集的会话，在重新收到合法服务端命令后自动恢复；STOP 和 SET_PARAM 会取消自动恢复。
- UDP 本地临时发送失败会保留未提交的采样子帧和控制回执，后续重试；有限采集缓冲在持续拥塞时仍可能溢出。

自研 UDP 服务端也必须定期发 GET_PARAM 等合法命令，否则板子会按超时机制重新注册。
这不是对每个 DATA 数据报做确认，不能通过保活判断某个 DATA 是否到达。

## 上位机启动

原 TCP EXE 不能直接与 UDP 固件通信，且本次没有覆盖旧 EXE 或原 Python GUI 文件。
新入口复用原 GUI 的命令、时域/FFT、归零、增益及 RAW/WAV/BIN 分包保存功能，只替换内置 Bridge。

优先运行本次生成的单文件 EXE：

`E:\AMY_ALL_CODE\ad_one_channel\build\udp-dist\采集器上位机_UDP.exe`

界面仍连接 `127.0.0.1:9001`。这是同机 GUI 与内置 Bridge 的兼容通道；板子到服务器的通信全部是 UDP。
设备服务器地址填写服务器实际可达 IP，设备端口填写 **UDP 9000**，不要把 127.0.0.1 写给板子。
服务器防火墙、云安全组及路由器端口映射需要放行/转发 UDP 9000；仅有 TCP 9000 规则无效。
运行前关闭占用 GUI TCP 9001 的旧上位机；退出新上位机会释放自己的端口。

### 多地址电脑的 UDP 回包源 IP

若同一网卡同时配置 `192.168.10.1` 和 `49.239.193.149`，而板子 server_ip 是后者，
不能仅依赖 `0.0.0.0:9000` 的默认 UDP 源地址选择。实板已确认：Windows 可能收到发给
49.239.193.149 的注册包，却从 192.168.10.1 发回 GET_PARAM，导致板子的已连接 UDP PCB
将其作为错误远端来源丢弃，表现为不断注册和 Peer timeout。

本机这种公网 IP 别名测试应明确绑定：

```powershell
& '.\采集器上位机_UDP.exe' --bind-ip 49.239.193.149 --device-id 3 --connect-on-start
```

成功启动后会记住监听 IP，保存在 `%APPDATA%\Channel1Acquisition\udp_gui.json`，
下次双击同一个 EXE 会沿用；`--connect-on-start` 仅控制本次自动连接本机 GUI Bridge。
需要恢复通配绑定时可运行 `--bind-ip 0.0.0.0`。

绑定地址必须实际属于本机。真实公网 NAT 部署时，应绑定服务器自身的本地地址并由 NAT 正确映射，
不能把未配置到服务器网卡上的公网 IP 直接用于 bind。独立 Bridge 同样支持 `--bind-ip`。
板端仍严格过滤配置的服务端 IP/端口，没有通过接受任意来源规避问题。

也可以从源码启动：

```powershell
& 'D:\pycharm\one_channel_tcp\.venv\Scripts\python.exe' tools/adc_udp_gui.py --device-id 3
```

换电脑时可加 `--gui-script 原adc_tcp_gui_5g_active_auto_bridge.py的路径`。
单文件 EXE 已包含打包时的原 GUI，不依赖外部 Python 文件。

Bridge 对 UDP 帧做 CRC 校验、最多 64 帧/20ms 有界重排并丢弃重复旧帧；缺失帧不补造，
最终仍可从原样点序号看出缺口。上位机持续过慢导致发送队列满时会关闭其 GUI 连接并记日志，
不会无限占内存或悄悄把多台设备的数据混成一条流。

### 多台设备

DATA 帧本来不含设备 ID，必须按 REGISTER ID＋UDP 源端点分流，不能把多台 DATA 混给同一个 GUI。
当前兼容 GUI 一次显示一台设备；`--device-id` 可固定接收某个 ID，省略时锁定首个注册 ID。
其他 ID 不会顶掉当前设备。无需修改协议即可同时启动不同端口的多个实例：

```powershell
& '.\采集器上位机_UDP.exe' --device-id 3 --device-port 9000 --gui-port 9001
& '.\采集器上位机_UDP.exe' --device-id 4 --device-port 9010 --gui-port 9011
```

对应将 ID=3 板子的 server_port 设为 9000，ID=4 板子设为 9010，并给同一局域网的板子配置不同本机 IP。
如果要求所有设备共用 UDP 9000、一个窗口同时显示多个设备，需要自研服务端按 ID/源端点分流；
不能在现有不带 DATA 设备 ID 的 GUI 流中直接混发。

独立 Bridge（供自研 TCP GUI 使用，不与内置 Bridge 同端口同时启动）：

```powershell
py tools/adc_udp_bridge.py --device-id 3 --device-port 9000 --gui-port 9001
```

## 编译与验证

固件：继续用原 Makefile。协议 CRC/打包、采样处理模块使用 -O2，其他模块保留原设置。

```powershell
py tests/test_udp_transport.py
py tests/test_udp_bridge.py
py tests/test_adc_dc_remove.py
```

测试依赖 ARM GCC 与 Unicorn，延用现有采样测试环境。UDP 固件状态机测试执行实际 Thumb 代码，
模拟 HAL/LwIP，覆盖本地发送失败重试、MTU 拆帧/样本连续性、START/STOP 顺序、非法包/错误源、
保活超时、网线恢复、网络命令以及 32 位时钟回绕。Bridge 测试使用本机真实 UDP/TCP socket。

初版离线验证：固件编译成功；8 项实际 Thumb 状态机测试、4 项 Bridge 测试及原去直流采样回归通过。
初版 EXE 曾在隔离的 UDP 19000/TCP 19001 端口验证注册、保活和转发；当时没有烧录实板。

2026-11-11 实板验证：修复多地址电脑的 UDP 回包源 IP，重新打包 EXE；6 项 Bridge 测试、
8 项状态机测试及去直流回归通过。直连以太网下，最高档 24 位连续 20 秒收到 7,773,600 个样点，
原始样本吞吐约 1.17 MB/s，CRC 错误、样点缺口及板端 overrun 均为 0。
另验证服务端中断后停采和恢复后自动续采。完整条件和边界见 [实板测试记录](udp_board_test_20261111.md)。
这不是实际 5G 网络或长期可靠性测试，也不代表 ADC 模拟精度通过。
UDP 不会增加运营商带宽，不保证无丢样。

重新打包 UDP GUI：

```powershell
& 'D:\pycharm\one_channel_tcp\.venv\Scripts\python.exe' -m PyInstaller --noconfirm --distpath build/udp-dist --workpath build/udp-gui tools/adc_udp_gui.spec
```

换电脑打包时设置环境变量 ADC_GUI_SCRIPT 指向原 GUI Python 文件。
