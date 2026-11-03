# 网口主动连接测试 Demo

只初始化 STM32 时钟、RMII、LAN8720A 和 LwIP，不初始化 ADC、SPI、增益控制或 Flash 参数。
复用项目原来的 Ethernet HAL 和收发驱动，Demo 独立管理 PHY 协商和 MAC 速率同步。
当前版本在启动时先自动进行三项环回自检，再恢复正常 PHY 模式并连接电脑。
环回结果保存到 `g_eth_loop_report`，方便通过调试器读取，不依赖 RTT 是否丢日志。

| 项目 | 配置 |
|---|---|
| 板子 | 192.168.11.100 / 255.255.255.0 |
| 电脑有线网卡 | 192.168.11.1 / 255.255.255.0 |
| 电脑 TCP 服务 | 192.168.11.1:5000 |
| 默认网关 | 直连不需要，留空 |
| 建连失败 | 5 秒超时，3 秒后重试 |
| 双向测试 | 每秒 BOARD_PING，电脑回复 PC_PONG |

## 编译

在项目根目录的 MSYS2 shell 执行：

```sh
make -f demos/eth_tcp_client/Makefile -j4 GCC_PATH=/c/Tools/gcc-arm-11.2-2022.02-mingw-w64-i686-arm-none-eabi/bin
```

输出独立存放在 `build/eth_tcp_client/eth_tcp_client.hex`、`.bin`、`.elf`、`.map`。
原项目 Makefile 和采集程序源码不变。

## 电脑服务

在电脑端运行（终端按 Ctrl+C 停止）：

```powershell
python demos/eth_tcp_client/pc_server.py
```

服务不发送采集指令，只响应测试文本。不要同时让其他程序占用 TCP 5000 端口。

## 预期结果

板子：

```text
[TCP OK] connected to PC 192.168.11.1:5000
[RX OK] PC_PONG count=1
```

电脑：

```text
CONNECTED ('192.168.11.100', ...)
RX ...: b'BOARD_PING\n'
TX ...: PC_PONG
```

`TX` 表示 HAL 完成的以太网发送数，不能单独证明电脑已收到；`RX` 表示 HAL 返回给 LwIP 的帧数。
只有 `PC_PONG` 计数持续增加，才能说明 TCP 双向收发已通过。
若始终建连超时，应继续检查 ARP、RMII 数据连线、50MHz 时钟和网口模拟链路；改变 TCP 连接方向不能绕过这些问题。

## 本次板上测试记录（2026-11-02）

固件已编译、烧录并运行；电脑服务绑定 `192.168.11.1:5000`，本机分段及连续心跳测试通过。
本机自检日志中的客户端地址是 `192.168.11.1`，不代表板子已连接。
首轮板上日志显示 PHY 可识别，但物理链路多次 UP/DOWN，接收帧和 PONG 均为 0，尚未通过双向通信测试：

```text
[PHY] addr=0 ID=0007:C0F1
[PHY] link UP, MAC 100M full duplex
[TCP] connect PC 192.168.11.1:5000
[STAT] link=1 BSR=782D PHY=1058 conn=0 TX=2 fail=0 RX=0 PONG=0
[PHY] link DOWN
[STAT] link=0 BSR=7809 PHY=0040 conn=0 TX=3 fail=0 RX=0 PONG=0
```

这说明该次测试没有成功建立 TCP 连接，不能据此单独认定某颗芯片损坏。
电脑日志位于 `build/eth_tcp_client/pc_server.log`，板上 RTT 快照位于同目录 `board_log.bin`。

## 恢复采集程序

烧录原采集固件即可。本次新板测试前另行保存了完整 Flash：
`E:/AMY_ALL_CODE/ad_one_channel_diagnostics/new_card_before_loopback_20261102_01.bin`，
加载地址为 `0x08000000`。对应芯片 UID 和测试记录见 `test_results_20261102.md`。
备份位于项目外，避免被 `make clean` 清除。
