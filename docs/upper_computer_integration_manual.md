# 单通道采集器上位机对接手册

本文档基于当前固件代码整理，用于上位机、服务器或调试工具对接下位机 TCP 数据流。

## 1. 通信模型

下位机不是 TCP Server，而是 TCP Client。

下位机启动后会主动连接：

```text
server_ip:server_port
```

当前固件默认网络参数：

```text
下位机本机 IP:   192.168.10.100
子网掩码:         255.255.255.0
网关:             192.168.10.1
远端服务器 IP:   49.239.193.149
远端服务器端口:  9000
默认设备 ID:      3
重连间隔:         3000 ms
TCP ACK 超时:     30000 ms
```

服务器侧需要监听 `49.239.193.149:9000` 对外可达的 TCP 端口。如果是本地电脑联调，需要通过 `SET_NET` 将下位机的 `server_ip` 改成电脑实际 IP。

## 2. 多设备接入

多个下位机可以同时连接同一个服务器端口。上位机或服务器需要通过设备 ID 区分连接。

设备连接成功后会主动发送一帧 `REGISTER`，服务器收到后应读取其中的 `device_id`，并建立映射：

```text
device_id -> TCP socket/connection
```

后续控制命令不再额外携带目标设备 ID，而是发到对应设备的 TCP 连接上。

设备 ID 规则：

```text
有效范围: 1..247
默认值:   3
要求:     同一服务器下同时在线设备 ID 必须唯一
```

如果服务器发现两个连接使用同一个 `device_id`，建议策略为：

```text
1. 保留最新连接，关闭旧连接；或
2. 拒绝新连接，并记录重复 ID 告警。
```

推荐使用第 1 种，方便设备断线重连后自动恢复。

## 3. 通用帧格式

所有协议帧都通过 TCP 字节流传输。TCP 没有天然消息边界，上位机必须按帧头中的 `payload_len` 进行拆包、粘包处理。

所有多字节整数均为小端序。

### 3.1 帧结构

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | magic | 固定 `0xA55A`，小端发送为 `5A A5` |
| 2 | 1 | version | 当前固定 `0x01` |
| 3 | 1 | type | 帧类型 |
| 4 | 2 | payload_len | payload 长度 |
| 6 | 4 | seq | 下位机/上位机各自发送时递增 |
| 10 | N | payload | 业务数据 |
| 10+N | 2 | crc16 | CRC16-Modbus，覆盖帧头和 payload |

CRC 计算范围：

```text
magic/version/type/payload_len/seq/payload
```

不包含最后 2 字节 CRC 本身。CRC 结果小端放入帧尾。

## 4. 帧类型

| type | 名称 | 方向 | 说明 |
|---:|---|---|---|
| `0x01` | `SET_PARAM` | 上位机 -> 下位机 | 设置采样参数 |
| `0x02` | `GET_PARAM` | 上位机 -> 下位机 | 查询采样参数 |
| `0x03` | `START` | 上位机 -> 下位机 | 开始采集 |
| `0x04` | `STOP` | 上位机 -> 下位机 | 停止采集 |
| `0x10` | `ACK` | 下位机 -> 上位机 | 参数/启动/停止响应 |
| `0x81` | `DATA` | 下位机 -> 上位机 | ADC 数据 |
| `0x20` | `SET_NET` | 上位机 -> 下位机 | 设置网络参数 |
| `0x21` | `GET_NET` | 上位机 -> 下位机 | 查询网络参数 |
| `0x22` | `NET_ACK` | 下位机 -> 上位机 | 网络参数响应 |
| `0x30` | `REGISTER` | 下位机 -> 上位机 | 连接成功后的注册帧 |

## 5. 连接注册帧 REGISTER

设备 TCP 连接成功后，会主动发送 `REGISTER`。

payload 长度固定 32 字节。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | device_id | 设备 ID |
| 1 | 3 | reserved | 保留 |
| 4 | 4 | fs_hz | 当前采样率 |
| 8 | 1 | bits | 当前数据位数，`16` 或 `24` |
| 9 | 1 | range | 保留字段，上位机不提供用户选择 |
| 10 | 1 | running | 当前是否采集中 |
| 11 | 1 | reserved | 保留 |
| 12 | 4 | sample_seq | 当前样本序号 |
| 16 | 4 | param_version | 参数版本 |
| 20 | 12 | reserved | 保留 |

上位机收到 `REGISTER` 后，应记录：

```text
device_id
TCP connection
当前采样率
当前位数
running 状态
```

## 6. 采样参数命令

### 6.1 GET_PARAM

方向：

```text
上位机 -> 下位机
```

payload 长度：

```text
0
```

下位机返回 `ACK`。

### 6.2 SET_PARAM

方向：

```text
上位机 -> 下位机
```

payload 长度至少 8 字节。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | fs_hz | 采样率 |
| 4 | 1 | bits | 数据位数 |
| 5 | 1 | range | 保留字段，上位机固定沿用当前值或填 `0` |
| 6 | 2 | reserved | 填 `0` |

支持采样率：

```text
400000
200000
100000
50000
25000
```

支持位数：

```text
24
16
```

注意：`range` 只是协议兼容字段，不建议在界面上暴露。发送 `SET_PARAM` 时建议先 `GET_PARAM`，然后沿用返回的 `range`；若无历史值，填 `0`。

下位机收到合法 `SET_PARAM` 后会：

```text
1. 停止当前采集
2. 更新采样配置
3. 保存到 Flash
4. 返回 ACK
```

## 7. 启停采集

### 7.1 START

方向：

```text
上位机 -> 下位机
```

payload 长度：

```text
0
```

下位机处理逻辑：

```text
1. 配置 ADC
2. 返回 ACK(status=0)
3. 等 START ACK 被 TCP 确认后，才真正开始采集
```

这样可以避免上位机还没收到启动响应，下位机就开始高速发送 DATA。

### 7.2 STOP

方向：

```text
上位机 -> 下位机
```

payload 长度：

```text
0
```

下位机停止采集后返回 `ACK`。如果停止时仍有半包数据，固件可能会先 flush 数据，再发送 STOP 的 ACK。

## 8. ACK 帧

`ACK` 用于响应 `SET_PARAM`、`GET_PARAM`、`START`、`STOP`。

payload 长度固定 44 字节。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | status | `int32`，`0` 表示成功，负数表示错误 |
| 4 | 4 | fs_hz | 当前采样率 |
| 8 | 1 | bits | 当前数据位数 |
| 9 | 1 | range | 保留字段 |
| 10 | 1 | running | 是否采集中 |
| 11 | 1 | reserved | 保留 |
| 12 | 4 | overrun | 丢样/溢出计数 |
| 16 | 4 | sample_seq | 当前样本序号 |
| 20 | 4 | flags | 流状态标志 |
| 24 | 2 | ready_count | READY 数据块数量 |
| 26 | 2 | queued_count | 已进入 TCP 队列的数据块数量 |
| 28 | 2 | free_count | 空闲数据块数量 |
| 30 | 2 | tcp_sndbuf | lwIP 当前发送缓冲剩余 |
| 32 | 4 | unacked_total_bytes | TCP 未确认总字节数 |
| 36 | 4 | tcp_err_mem | TCP 内存/发送空间不足计数 |
| 40 | 4 | acked_data_frames | 已被 TCP ACK 的 DATA 帧数 |

常用 `status`：

| status | 含义 |
|---:|---|
| `0` | 成功 |
| `-10` | `SET_PARAM` 长度非法 |
| `-11` | `SET_PARAM` 参数非法 |
| `-12` | ADC 配置失败 |
| `-13` | 参数保存失败 |
| `-14` | 量程/前端控制失败 |
| `-20` | `START` ADC 配置失败 |
| `-21` | `START` 启动采集失败 |
| `-22` | 存在旧 TCP 数据未确认 |
| `-23` | `START` 前端控制失败 |

## 9. DATA 帧

`DATA` 为下位机采样数据帧。

payload 由 16 字节 meta 加 ADC 原始数据组成。

### 9.1 DATA meta

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | fs_hz | 本帧采样率 |
| 4 | 1 | bits | 数据位数 |
| 5 | 1 | range | 保留字段 |
| 6 | 1 | bytes_per_sample | 每点字节数，24bit 为 3，16bit 为 2 |
| 7 | 1 | reserved | 保留 |
| 8 | 4 | first_sample_seq | 本帧第一个样本序号 |
| 12 | 2 | sample_count | 本帧样本数 |
| 14 | 2 | data_bytes | ADC 数据字节数 |

### 9.2 ADC 数据区

数据区从 payload 偏移 16 开始：

```text
payload[16 .. 16 + data_bytes - 1]
```

当前每包默认样本数：

```text
APP_ADC_BLOCK_SAMPLES = 790
```

因此典型 DATA 帧长度：

```text
24bit: 10 + 16 + 790*3 + 2 = 2398 字节
16bit: 10 + 16 + 790*2 + 2 = 1608 字节
```

上位机应使用 `bytes_per_sample` 和 `sample_count` 解析，不要写死长度。

样本序号 `first_sample_seq` 为 32 位无符号计数，长时间运行会回绕。上位机判断连续性时应按 uint32 回绕处理。

## 10. 网络参数命令

### 10.1 GET_NET

方向：

```text
上位机 -> 下位机
```

payload 长度：

```text
0
```

下位机返回 `NET_ACK`。

### 10.2 SET_NET

方向：

```text
上位机 -> 下位机
```

支持两种 payload 长度。

#### 8 字节格式

用于只修改远端服务器和设备 ID。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | server_ip | 服务器 IPv4 |
| 4 | 2 | server_port | 服务器端口 |
| 6 | 1 | device_id | 设备 ID |
| 7 | 1 | flags | 控制标志 |

#### 28 字节格式

用于本地联调或完整网络参数修改。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | server_ip | 服务器 IPv4 |
| 4 | 2 | server_port | 服务器端口 |
| 6 | 1 | device_id | 设备 ID |
| 7 | 1 | flags | 控制标志 |
| 8 | 4 | local_ip | 下位机本机 IPv4 |
| 12 | 4 | netmask | 子网掩码 |
| 16 | 4 | gateway | 网关 |
| 20 | 4 | reconnect_ms | 重连间隔 |
| 24 | 4 | reserved | 填 `0` |

flags：

| bit | 值 | 说明 |
|---:|---:|---|
| bit0 | `0x01` | 保存到 Flash |
| bit1 | `0x02` | `NET_ACK` 被 TCP 确认后，断开并按新参数重连 |

常用 flags：

```text
0x00: 只临时修改 RAM 参数，不保存，不立即重连
0x01: 保存到 Flash，但不立即重连
0x03: 保存到 Flash，并在 NET_ACK 确认后立即重连
```

设置网络参数时，固件要求当前不能处于采集运行状态。建议流程：

```text
1. 发送 STOP
2. 等 ACK
3. 发送 SET_NET
4. 等 NET_ACK
5. 如果 flags 带 0x02，等待设备断开并重新连接
6. 收到新的 REGISTER
```

### 10.3 NET_ACK

payload 长度固定 36 字节。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | status | `int32`，`0` 表示成功 |
| 4 | 4 | server_ip | 当前服务器 IPv4 |
| 8 | 2 | server_port | 当前服务器端口 |
| 10 | 1 | device_id | 当前设备 ID |
| 11 | 1 | connected | 当前 TCP 是否连接 |
| 12 | 1 | reconnect_pending | 是否等待重连 |
| 13 | 3 | reserved | 保留 |
| 16 | 4 | local_ip | 下位机本机 IP |
| 20 | 4 | netmask | 子网掩码 |
| 24 | 4 | gateway | 网关 |
| 28 | 4 | reconnect_ms | 重连间隔 |
| 32 | 4 | reserved | 保留 |

常用 `status`：

| status | 含义 |
|---:|---|
| `0` | 成功 |
| `-30` | `SET_NET` 长度非法 |
| `-31` | 采集中，需先 STOP |
| `-32` | 网络参数非法 |
| `-33` | 保存 Flash 失败 |

## 11. 断线和重连行为

下位机主动连接服务器。连接失败或断开后，会按 `reconnect_ms` 间隔重连。

当前固件还有 TCP ACK 看门狗：

```text
如果存在未确认 TCP 数据，且 30000 ms 内没有收到 TCP ACK 进展，
下位机会停止采集，abort 当前 TCP 连接，并进入重连。
```

这用于处理网关外网断流、服务器不可达、网络中间链路异常但本地 PHY link 仍然 up 的情况。

注意：LAN8720 PHY link 只能检测本地网线/网关 LAN 口是否连通，不能判断远端服务器或运营商流量是否可用。

## 12. 本地设置教程

### 12.1 当前默认远端服务器模式

当前固件默认连接：

```text
49.239.193.149:9000
```

正式联调时，需要确认：

```text
1. 服务器公网 IP 为 49.239.193.149，或该 IP 已映射到服务器。
2. 服务器 TCP 端口 9000 已监听。
3. 云服务器/防火墙/安全组已放行 TCP 9000。
4. 下位机所在网关可访问公网。
5. 下位机局域网配置为：
   local_ip = 192.168.10.100
   netmask  = 255.255.255.0
   gateway  = 192.168.10.1
```

服务器程序启动后，等待设备主动连接。连接成功后，第一帧应为 `REGISTER`。

### 12.2 本地电脑联调模式

如果不用公网服务器，而是电脑直接做服务器：

```text
电脑 IP: 192.168.10.2
电脑监听端口: 9000
下位机 IP: 192.168.10.100
子网掩码: 255.255.255.0
网关: 192.168.10.1
```

电脑侧操作：

```text
1. 将电脑网卡设置到 192.168.10.x 网段，例如 192.168.10.2。
2. 关闭或放行防火墙中的 TCP 9000。
3. 启动 TCP Server，监听 0.0.0.0:9000 或 192.168.10.2:9000。
```

下位机侧需要通过 `SET_NET` 改成：

```text
server_ip   = 192.168.10.2
server_port = 9000
device_id   = 按设备编号填写，例如 1、2、3...
local_ip    = 每台设备唯一，例如 192.168.10.101、192.168.10.102...
netmask     = 255.255.255.0
gateway     = 192.168.10.1
reconnect   = 3000
flags       = 0x03
```

多台设备本地联调时，必须保证：

```text
1. 每台设备 local_ip 唯一。
2. 每台设备 device_id 唯一。
3. 所有设备 server_ip 都指向同一台电脑服务器 IP。
4. 服务器按 REGISTER 中的 device_id 区分设备。
```

### 12.3 从本地模式切回当前远端服务器

发送 `SET_NET`，使用 28 字节格式：

```text
server_ip   = 49.239.193.149
server_port = 9000
device_id   = 当前设备编号
local_ip    = 192.168.10.100 或该设备规划 IP
netmask     = 255.255.255.0
gateway     = 192.168.10.1
reconnect   = 3000
flags       = 0x03
```

收到 `NET_ACK(status=0)` 后，设备会断开当前 TCP，再按新服务器地址重连。服务器应等待新的 `REGISTER`。

## 13. 上位机推荐处理流程

单台设备：

```text
1. 启动 TCP Server。
2. 等待设备连接。
3. 收到 REGISTER，记录 device_id。
4. 发送 GET_PARAM，确认当前采样参数。
5. 需要修改采样率/位数时，发送 SET_PARAM。
6. 发送 START。
7. 持续接收 DATA。
8. 停止时发送 STOP。
```

多台设备：

```text
1. 服务器监听同一个 TCP 端口。
2. 每来一个 TCP 连接，先等待 REGISTER。
3. 按 REGISTER.device_id 建立连接表。
4. 控制某台设备时，向该 device_id 对应的 socket 发送命令。
5. DATA 入库或显示时，使用 socket 对应的 device_id 作为数据来源。
6. 如果连接断开，移除该 device_id 的在线状态，等待重连 REGISTER。
```

## 14. Python 组帧示例

```python
import struct

MAGIC = 0xA55A
VER = 0x01

def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc

def build_frame(frame_type: int, seq: int, payload: bytes = b"") -> bytes:
    header = struct.pack("<HBBHI", MAGIC, VER, frame_type, len(payload), seq)
    body = header + payload
    crc = crc16_modbus(body)
    return body + struct.pack("<H", crc)

def build_start(seq: int) -> bytes:
    return build_frame(0x03, seq)

def build_stop(seq: int) -> bytes:
    return build_frame(0x04, seq)

def build_get_param(seq: int) -> bytes:
    return build_frame(0x02, seq)

def build_set_param(seq: int, fs_hz: int, bits: int, range_value: int = 0) -> bytes:
    payload = struct.pack("<IBBBB", fs_hz, bits, range_value, 0, 0)
    return build_frame(0x01, seq, payload)

def build_set_net_28(seq: int, server_ip, server_port, device_id,
                     local_ip, netmask, gateway, reconnect_ms=3000,
                     flags=0x03) -> bytes:
    payload = bytes(server_ip)
    payload += struct.pack("<HBB", server_port, device_id, flags)
    payload += bytes(local_ip)
    payload += bytes(netmask)
    payload += bytes(gateway)
    payload += struct.pack("<II", reconnect_ms, 0)
    return build_frame(0x20, seq, payload)
```

## 15. 上位机解析注意事项

```text
1. TCP 必须做缓存拆包，不能假设 recv 一次就是一帧。
2. 先找 magic=0xA55A，再判断 version、payload_len、CRC。
3. DATA 帧用 payload_len、sample_count、data_bytes 校验长度。
4. 多设备时，命令通过 socket 路由，不通过广播。
5. device_id 必须唯一。
6. sample_seq 是 uint32，会回绕。
7. 网络异常时设备可能主动断开并重连，服务器应允许同一 device_id 重新 REGISTER。
```
