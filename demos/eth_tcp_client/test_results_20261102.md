# 当前板卡网口分段测试记录

日期：2026-11-02。用户更换板卡后重新测试，以下结果仅对应本次板卡。

- STM32 UID 原始三个 32 位字：`00360047 35325112 36383231`。
- 完整 Flash 备份：`E:/AMY_ALL_CODE/ad_one_channel_diagnostics/new_card_before_loopback_20261102_01.bin`，大小 1048576 字节，起始地址 `0x08000000`。
- 当前板上程序：独立 Ethernet TCP Client Demo，不启动 ADC，不写 Flash 参数。
- 电脑：`192.168.11.1/24`，监听 TCP 5000。
- 板子：`192.168.11.100/24`，网关为 0，直连电脑。
- Demo 烧录后逐字节校验通过；BIN 以 0xFF 填充段间空洞，以匹配 HEX 的 Flash 内容。

## 环回结果

每档发送 8 个 60 字节测试帧，检查收回帧的前 25 字节，包括 MAC 地址、EtherType、标识、阶段编号和序号。

| 阶段 | PHY BCR | MAC 内部环回 | 发送成功 | 收回内容匹配 | HAL 错误 |
|---|---|---|---|---|---|
| MAC 100M | 0x2100 | 开启 | 8/8 | 8/8 | 0 |
| PHY 100M 近端环回 | 0x6100 | 关闭 | 8/8 | 8/8 | 0 |
| PHY 10M 近端环回 | 0x4100 | 关闭 | 8/8 | 8/8 | 0 |

原始 BCR 为 0x3000；测试后恢复原配置并重新启动自协商，恢复错误码为 0。
正常网络收发计数在环回后清零，表中的环回帧不计入后续 RX。

环回说明依据 Microchip LAN8720A 数据手册 3.8.8：
[官方手册](https://ww1.microchip.com/downloads/aemDocuments/documents/UNG/ProductDocuments/DataSheets/LAN8720A-LAN8720Ai-Data-Sheet-DS00002165.pdf)。
PHY 近端环回经过 RMII 和 PHY 内部数字通路，不验证网络变压器、RJ45、外部网线及外部模拟收发路径。

## 正常网络测试

启动阶段记录了 4 次 link up、3 次 link down，随后稳定到 10M 全双工。
在约 120~134 秒时这些次数不再增长，不能把启动阶段的掉链描述为持续掉链。

```text
[STAT] ms=134000 UP=4 DOWN=3 link=1 BSR=782D PHY=1054 conn=0 TX=127 fail=0 RX=0 PONG=0 MACCR=02008E0C DMA=00664506
```

电脑已经学到板子 MAC `00-80-E1-00-00-00`，有线网卡收到的广播计数增加，但 TCP 服务没有收到来自板子的连接。
电脑以 `192.168.11.1` 为源地址 ping 板子，4 个请求全部超时。

另从电脑向 `192.168.11.255:5001` 发送 64 个 UDP 广播，不依赖 ARP 解析：

- 电脑有线网卡发送广播计数恰好增加 64。
- 板子正常模式的 `rx_frames` 仍为 0。
- 板子随后累计发送计数 200，HAL 发送失败 0，PONG 为 0。
- CFSR/HFSR 均为 0，程序仍在运行。

HAL 发送成功只说明 DMA 完成发送，不等同于对端收到。电脑驱动的发送计数也不单独证明线上信号正确。

## 结论与下一步

MAC/DMA、基础收发驱动、RMII 数字连线和 PHY 内部数字通路通过了本次短帧基本功能测试。
现阶段更应检查电脑到板子的外部物理接收方向：网线/电脑网口发送、RJ45、变压器接线与型号、终端阻容、PHY 模拟接收供电及焊接。
这些结果没有单独证明 PHY 芯片损坏，也不代表所有负载和温度条件下均无问题。

建议先换一根已知正常的短网线；仍异常时，在持续发包过程中用示波器逐级观察 RJ45/变压器/PHY 侧差分信号，断电检查绕组及串联器件通断。

原始记录统一保存在 `E:/AMY_ALL_CODE/ad_one_channel_diagnostics/`：

- `new_card_loopback_jlink_20261102_01.txt`：UID、Flash 校验和环回结果内存。
- `new_card_loopback_boot_20261102_01.bin`：启动 RTT 快照。
- `new_card_monitor_jlink_20261102_01.txt`、`new_card_monitor_20261102_01.bin`：运行后计数和 RTT 环形缓冲快照。
- `new_card_after_broadcast_jlink_20261102_01.txt`：广播测试后的 RX、PHY 模式及故障寄存器。
- `pc_loopback_test_20261102_01.log`：电脑端 TCP 服务日志。

电脑测试服务此次设置为启动 15 分钟后自动退出。需继续测试时重新运行 `py demos/eth_tcp_client/pc_server.py`。
