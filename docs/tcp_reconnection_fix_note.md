# TCP 断联检测与自动重连修复说明

## 1. 问题描述

设备采用主动 TCP 客户端模式，连接配置的上位机服务器。在原逻辑中，TCP 错误回调和发送数据时的 ACK 超时可以触发重连，但存在两个空闲连接场景没有完整覆盖：

1. TCP 端口已经连接，双方暂时没有业务数据时，拔掉网线虽然 PHY 能检测到链路断开，但已有 TCP PCB 不会被主动释放。
2. 网线仍然连接，但网关、路由或远端服务器网络失联时，PHY 仍显示 link up。由于连接中没有待确认数据，发送 ACK 看门狗不会运行，旧连接可能长期停留在“已连接”状态。

这会造成设备保存着已经失效的 TCP 连接，不再进入主动连接流程，即“假连接”。

## 2. 修复目标

修复后覆盖以下场景：

- 数据发送期间网络中断；
- 只连接端口、没有业务数据时网络中断；
- 设备侧网线拔出及重新插入；
- 上位机正常关闭 TCP；
- 上位机、网关或中间网络异常失联；
- 网络恢复或服务器恢复监听后自动重连；
- 断线前正在采集时，重连成功后自动恢复采集。

## 3. 修改内容

### 3.1 物理链路断开处理

`AdcTcpServer_Task()` 每轮检查 LwIP 网口的管理状态和 PHY link 状态。

检测到 link down 后执行：

1. 停止 ADC 采集；
2. 保存断线前是否存在采集请求；
3. 清理接收状态和 TCP 发送队列；
4. 终止已经连接或正在连接的 TCP PCB；
5. 在 PHY 链路恢复前暂停连接尝试；
6. 链路恢复后重新连接服务器。

PHY 状态当前每 100 ms 检查一次，因此本地网线断开通常可以在约 100 ms 内被发现。

相关代码：`device/adc_tcp_server.c`、`LWIP/App/lwip.c` 和 `LWIP/Target/ethernetif.c`。

### 3.2 空闲连接 TCP keepalive

LwIP 已启用 TCP keepalive，并在每次 TCP 连接成功后配置：

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `APP_TCP_KEEPALIVE_IDLE_MS` | 10000 ms | TCP 空闲 10 秒后开始探测 |
| `APP_TCP_KEEPALIVE_INTERVAL_MS` | 5000 ms | 每隔 5 秒发送一次探测 |
| `APP_TCP_KEEPALIVE_PROBES` | 3 | 连续 3 次无响应判定连接失效 |

网线物理状态仍正常，但网关或远端网络失联时，通常在约 25～30 秒内触发 TCP 错误回调。错误回调会停止采集、释放旧连接，并在 `reconnect_ms` 到期后重新连接。

相关代码：`LWIP/Target/lwipopts.h`、`device/app_net_config.h` 和 `device/adc_tcp_server.c`。

### 3.3 数据发送阶段 ACK 超时

原有发送 ACK 看门狗继续保留。存在未确认 TCP 数据，并且连续 30 秒没有任何 ACK 进展时：

1. 停止采集；
2. 强制终止当前 TCP；
3. 清理未确认发送队列；
4. 进入重连流程。

该机制处理采集数据持续发送期间的网络堵塞、网关流量耗尽或远端不可达问题。

### 3.4 上位机主动关闭连接

上位机正常关闭 TCP 时，LwIP 接收回调会收到关闭事件。设备随后停止采集、关闭旧连接并进入重连流程，无需等待 keepalive 超时。

### 3.5 重连后的采集恢复

设备用 `s_stream_requested` 保存上位机是否发出过 START 且尚未发出 STOP。

断线前正在采集时：

1. 断线检测后立即停止实际采集；
2. TCP 重连成功后发送 REGISTER 和状态 ACK；
3. 等待这些控制帧获得 TCP 确认；
4. 重新启动 ADC 采集和数据发送。

断线前未处于采集请求状态时，重连后保持停止状态，等待上位机重新发送 START。

## 4. 重连时序

### 物理网线断开

`PHY link down -> 停止采集 -> 终止旧 TCP -> 等待 link up -> 发起 TCP 连接`

### 网关或远端网络失联

`TCP 空闲 -> keepalive 探测 -> 连续 3 次无响应 -> TCP 错误回调 -> 等待 reconnect_ms -> 发起 TCP 连接`

### 发送数据期间失联

`存在未确认数据 -> 30 秒无 ACK 进展 -> 停止采集 -> 终止旧 TCP -> 发起重连`

默认 `reconnect_ms` 为 3000 ms，可通过网络参数配置，允许范围为 500～60000 ms。

## 5. 建议验证项目

1. 只建立 TCP 连接，不发送 START，拔掉设备网线后重新插入，确认设备重新连接服务器。
2. 只建立 TCP 连接，通过断开网关或上位机网络制造远端失联，确认约 25～30 秒后进入重连。
3. 正在采集时拔线，确认采集立即停止；恢复网络后确认重新注册并自动恢复采集。
4. 上位机主动关闭 socket，确认设备进入重连，并在服务器重新监听后恢复连接。
5. 保持物理网线连接但停止服务器，确认设备持续按 `reconnect_ms` 尝试连接。
6. 上位机发送 STOP 后再断网，确认重连后不会自动启动采集。

建议观察以下日志：

```text
[TCP FAIL] Ethernet link down: stop stream, abort TCP and wait for link
[TCP FAIL] TCP error callback: err=...
[TCP] TCP client connect attempt #...
[TCP OK] TCP connected to server: ...
[TCP] AUTO RESUME: wait REGISTER/ACK TCP confirmation before sampling
```

## 6. 使用边界

TCP keepalive用于判断对端 TCP 协议栈或网络是否可达。如果上位机应用程序卡死，但上位机操作系统和 socket 仍能正常响应 TCP keepalive，设备仍会认为连接有效。若需要检测上位机业务程序是否正常运行，协议还需要增加应用层心跳请求和心跳响应。

## 7. 修改文件

- `LWIP/Target/lwipopts.h`：启用可配置 TCP keepalive。
- `device/app_net_config.h`：增加 keepalive 时间和次数配置。
- `device/adc_tcp_server.c`：增加物理链路检查、空闲连接 keepalive配置，以及建连中 PCB 的断链清理。

