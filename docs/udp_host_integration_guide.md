# 单通道采集器：TCP 转 UDP 上位机对接说明

版本日期：2026-11-11  
适用固件：`ad_one_channel` 项目，`udp_client` 分支当前实现。  
读者：上位机、采集服务器、设备接入服务开发人员。

本文以原“设备主动连接服务器”的 TCP 客户端版本为迁移基线，不是 master 的 TCP 服务端模式。
本版设备与服务器之间的注册、控制、回执、采样数据全部使用 UDP。
应用帧版本仍为 1，未新增命令码或可靠 UDP 层，不能只按 version 区分 TCP/UDP 固件。

## 1. 迁移结论与必改清单

原有命令编码和大部分 payload 解码可复用，但不能只把 SOCK_STREAM 换成 SOCK_DGRAM。

| 项目 | 原 TCP 主动连接版本 | 当前 UDP 版本：上位机需要如何修改 |
| --- | --- | --- |
| 服务端入口 | listen/accept，设备主动 connect | bind UDP 监听端口，通过 recvfrom 接收 |
| 默认设备目的端口 | TCP 9000 | UDP 9000；云安全组、防火墙、NAT 必须放行 UDP |
| 设备标识与目标 | device_id -> TCP 连接 | device_id -> 最近确认的 UDP 源 IP/源端口 |
| 消息边界 | 字节流，要处理拆包和粘包 | 一报一帧，严格校验本报长度；不跨数据报拼帧 |
| 帧头、类型、CRC | 原二进制格式 | 不变，详见第 4 节 |
| DATA 样点数 | 内部完整块通常 790 点，停止时可有部分块 | 每块可拆成多个 DATA，必须读取 sample_count/data_bytes |
| 可靠性 | 同一连接内按序、可靠字节流 | 可能丢失、重复、乱序；固件不做远端丢包重传 |
| 空闲保活 | TCP keepalive 等传输层机制 | 服务端必须向每台设备定期发合法命令，推荐 GET_PARAM 每 5 秒一次 |
| 失联检测 | TCP 错误、连接关闭、ACK 超时 | 固件 30 秒未收到合法服务端命令即清理会话并重新注册 |
| 发送成功 | TCP 栈另有传输层 ACK | sendto 成功仅表示本地提交，不表示设备或服务器已收到 |
| START 时序 | 等待 START 响应被 TCP 确认后开采 | 本地提交 START ACK 后开采，不等远端确认 |
| STOP 时序 | 有序字节流承载剩余 DATA 和 ACK | 本地先提交剩余 DATA 再提交 STOP ACK；到达端仍可能乱序/丢失 |
| 网络专属统计 | sndbuf、unacked、已 TCP 确认 DATA 数 | 这些字段为 0，不能用于判定 UDP 是否全部送达 |

上位机必须实现：UDP 收发、源端点登记、逐设备保活、可变 DATA 解码、丢包/乱序/重复处理、
超时状态管理以及控制命令串行化。绘图、FFT、文件保存应复用已有样本处理层，但要记录缺口。

## 2. 网络接入、回包与多设备

### 2.1 当前默认参数

```text
设备本机 IP：192.168.10.100
掩码：255.255.255.0
网关：192.168.10.1
服务器 IP：49.239.193.149
服务器端口：UDP 9000
设备 ID：3（有效范围 1..247）
注册/重试间隔：reconnect_ms，默认 3000 ms
合法服务端命令接收超时：30000 ms
```

Flash 中保存的参数优先于默认值，应以本次设备日志或 GET_NET 回执为准。
目的 IP 不在设备网段时，设备经配置网关发送；网关必须提供实际可达的路由/NAT。
设备本机 UDP 源端口动态分配，重建会话时可以变化，不能写死为 9000 或 625xx。

### 2.2 正确的服务器收发方式

1. 服务器创建 UDP socket，绑定可达的本地地址和端口，例如 UDP 9000。
2. recvfrom 收到有效 REGISTER 后，读取 device_id，保存本次观察到的源端点。
3. 使用同一个绑定 socket，向该源端点 sendto 命令；不要创建随机源端口的回包 socket。
4. 服务端在设备看到的回包源 IP/端口必须等于设备配置的 server_ip/server_port。
5. 服务器继续接收所有设备的数据报，按源端点反查 device_id 后分发。

设备严格过滤服务端来源。不能“向 49.239.193.149:9000 收包，却从另一个地址或端口回包”。
多网卡/多地址服务器要固定正确的 UDP 回包源地址；不是修改命令 payload 中的 IP。

当前电脑联调同时配置了 `192.168.10.1` 与 `49.239.193.149`，应明确绑定
`49.239.193.149:9000`，避免 Windows 从 `192.168.10.1` 回包而被设备拒收。
真实公网 NAT 部署时，服务器 bind 的应是其实际拥有的本地地址，由 NAT 映射为正确的公网来源；
不要 bind 一个服务器网卡上不存在的公网 IP。

### 2.3 多设备分流

自研服务器可以使用一个 UDP 9000 接入多个不同 ID：

```text
设备表：device_id -> {peer_ip, peer_port, last_rx, current_config, stream_epoch, counters}
反向表：(peer_ip, peer_port) -> device_id
```

- ID 必须唯一，同一局域网内设备本机 IP 也必须唯一；经同一 NAT 的设备不能仅按 IP 区分。
- 命令、DATA、ACK 都没有额外的目标/来源设备 ID；只有 REGISTER、NET_ACK 含 device_id。
- DATA 不能直接从 payload 识别设备，必须通过已登记的源端点归属。
- 未登记来源的 DATA 不要混入任何已有设备；等待 REGISTER 或隔离记录。
- 已登记端点的周期 REGISTER 是正常保活，不应每 3 秒清空波形或重置丢包计数。
- 相同 ID 换端口可能是重连或 NAT 映射变化。接受新 REGISTER 后应原子更新双向映射，
  隔离旧端点和迟到包，并建立新的会话上下文；两个端点持续同时注册同一 ID 时应告警。
- 协议没有认证、设备唯一硬件标识或会话令牌，不能可靠区分重复 ID、伪造注册和所有迟到旧包。
  ID 冲突策略必须明确，不能无声地把两块板的数据拼在一起。

配套兼容 GUI 一次只显示一个 ID；它不是完整的多设备界面。
自研多设备服务器应为每台设备独立保存保活、控制队列、重排和采集状态。

## 3. 保活、离线与重新注册

### 3.1 不操作也必须有通信

用户只打开连接、不点 START 时，服务器仍必须对每台已注册设备定时发送 GET_PARAM。
推荐周期 5 秒，收到 ACK 后更新状态；无需逐包回复 DATA。

```text
设备 -> 服务器：REGISTER（启动及周期发送，默认每 3 秒）
服务器 -> 设备：GET_PARAM（收到注册后尽快发送，以后约每 5 秒）
设备 -> 服务器：ACK
用户需要时：SET_PARAM / START / STOP / GET_NET / SET_NET
```

收到 REGISTER 仅说明上行路径可达，不证明设备已接受服务器回包。
应在收到所发查询对应的状态反馈后确认双向通信，不能只凭“UDP socket 已建立”显示在线。
服务器自身也应设每台设备的接收超时并标记离线，例如 30 秒未收到有效设备帧；
这是上位机要实现的策略，不是操作系统 UDP 自动提供的通知。

### 3.2 当前固件的恢复规则

- 收到正确服务端来源、CRC/帧格式有效、类型为已知控制命令的报文后，设备更新在线计时。
- 收到 DATA 的服务端无需回传应用 ACK；伪造 TCP ACK 或发送任意字符串不能保活。
- 包括待机和采集中，设备连续 30 秒未收到合法命令会停采、丢弃会话内待发数据并移除 UDP PCB。
- 默认约 3 秒后重建会话并注册，等待期间仍受物理链路状态控制；可能换源端口。
- 本地 PHY 链路按约 100 ms 周期检测，发现断链会清理会话；恢复后重新注册。
- 中断前在采集，设备在重新收到合法服务端命令后会自动续采；此前未采集则继续待机。
- STOP、有效 SET_PARAM 会取消采集请求/自动续采；服务器重启后不要一律额外发 START。
- 自动续采重新开始计数，不是无损接续；网络中断期间的样本不会补传。

GET_PARAM 本身可能触发待恢复会话续采，查询回执生成时 running 还可能为 0，随后才出现 DATA。
若服务器重启后业务要求保持停采，应先发 STOP 并查询确认，而不是只依靠 GET_PARAM。
上位机程序退出后必须重新运行服务，设备不能替电脑启动软件。

## 4. 应用帧格式：保持不变

UDP payload 中是一整个应用帧，不含以太网/IP/UDP 头。
所有下表整数为小端；IP 地址按四个字节顺序存放；ADC 样本字节序例外，见第 6 节。

| 应用帧偏移 | 长度 | 字段 | 说明 |
| ---: | ---: | --- | --- |
| 0 | 2 | magic | 0xA55A，线上字节 5A A5 |
| 2 | 1 | version | 0x01 |
| 3 | 1 | type | 见下表 |
| 4 | 2 | payload_len | 不含 10 字节帧头和 2 字节 CRC |
| 6 | 4 | seq | 发送方自己的应用帧序号，uint32 |
| 10 | N | payload | 内容随 type 变化 |
| 10+N | 2 | crc16 | CRC16/Modbus，低字节在前 |

CRC：初始值 0xFFFF，反射多项式 0xA001，最终不异或，覆盖全部帧头及 payload，不含 CRC 自身。
校验向量：ASCII `123456789` 的 CRC 为 0x4B37。

每次 recvfrom 必须检查：至少 12 字节、magic/version、实际长度等于 payload_len+12、CRC、
类型及对应 payload 结构。错误或截断报文整报丢弃，不与下一报拼接。
当前固件接收的完整控制帧上限 256 字节；设备发出的单个 UDP payload 上限 1252 字节。
服务器可用 65535 字节接收缓冲，避免 API 缓冲过小造成截断；业务队列也应有界并统计溢出。

| type | 名称 | 方向 | payload 长度 |
| --- | --- | --- | --- |
| 0x01 | SET_PARAM | 服务器 -> 设备 | 规范发送 8 字节 |
| 0x02 | GET_PARAM | 服务器 -> 设备 | 0 |
| 0x03 | START | 服务器 -> 设备 | 0 |
| 0x04 | STOP | 服务器 -> 设备 | 0 |
| 0x10 | ACK | 设备 -> 服务器 | 44 |
| 0x20 | SET_NET | 服务器 -> 设备 | 规范发送 8 或 28 字节 |
| 0x21 | GET_NET | 服务器 -> 设备 | 0 |
| 0x22 | NET_ACK | 设备 -> 服务器 | 36 |
| 0x30 | REGISTER | 设备 -> 服务器 | 32 |
| 0x81 | DATA | 设备 -> 服务器 | 16 + data_bytes |

一个 sendto 只放一帧。不能把多个命令拼成一个数据报，也不能将一条命令拆成几个数据报。

## 5. 控制、注册与回执字段

以下偏移均从 payload 起点计算，不是整个 UDP payload 的起点；还需加 10 字节应用帧头。
保留字段发送时置 0，接收时忽略。range 字段保留原布局，普通对接不增加自选增益功能，
SET_PARAM 默认发 0，已有设备参数按回读保留；不要删掉该字节导致后续字段错位。

### 5.1 REGISTER：32 字节

| payload 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 1 | device_id，1..247 |
| 1 | 3 | 保留 |
| 4 | 4 | fs_hz，配置档位值 |
| 8 | 1 | bits，16 或 24 |
| 9 | 1 | range，兼容字段 |
| 10 | 1 | running，0/1 |
| 11 | 1 | 保留 |
| 12 | 4 | sample_seq，当前样点计数 |
| 16 | 4 | param_version，当前 0x0001000A，参数记录版本而非 UDP 会话 ID |
| 20 | 12 | 保留 |

### 5.2 SET_PARAM：8 字节；GET_PARAM/START/STOP：空 payload

| payload 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | fs_hz：400000、200000、100000、50000、25000 之一 |
| 4 | 1 | bits：16 或 24 |
| 5 | 1 | range，保留兼容字段，默认 0 |
| 6 | 2 | 保留，置 0 |

SET_PARAM 会停采、应用参数并保存 Flash，不是只修改内存。
心跳禁止使用 SET_PARAM，也不要在超时后无限重发它，以免反复擦写 Flash。

### 5.3 ACK：44 字节

| payload 偏移 | 长度 | 字段 | UDP 含义 |
| ---: | ---: | --- | --- |
| 0 | 4 | status，int32 | 0 成功，负值失败 |
| 4 | 4 | fs_hz | 配置档位，不是 DATA 的实际采样率 |
| 8 | 1 | bits | 当前位数 |
| 9 | 1 | range | 兼容字段 |
| 10 | 1 | running | 生成本回执时是否采集中 |
| 11 | 1 | reserved | 0 |
| 12 | 4 | overrun | 板端因缓冲等原因丢弃的样点累计数，不是网络丢包数 |
| 16 | 4 | sample_seq | 当前样点计数 |
| 20 | 4 | flags | 原采样状态位，保留布局 |
| 24 | 2 | ready | 板端 READY 块数 |
| 26 | 2 | queued | 原 QUEUED 块统计，不代表远端已收到 |
| 28 | 2 | free | 板端空闲块数 |
| 30 | 2 | sndbuf | 固定 0，无 TCP 发送窗口 |
| 32 | 4 | unacked | 固定 0，无 TCP 未确认字节数 |
| 36 | 4 | 原 tcp_err_mem | 改为 UDP 本地发送错误累计数，可能含内存、路由等本地错误 |
| 40 | 4 | 原 tcp_acked_data_frames | 固定 0，无逐 DATA 远端确认 |

常见 flags：0x200 为 DMA_STARTED，不是故障；0x100 为等待首次 DRDY；0x80 为 ADC 状态异常；
0x1 为采集溢出标志。保留原状态位解码，不要将任意非零 flags 都显示成网络断开。

### 5.4 SET_NET：8 或 28 字节

| payload 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | server_ip，四个 IPv4 字节 |
| 4 | 2 | server_port，小端；有效 1..65534 |
| 6 | 1 | device_id，1..247 |
| 7 | 1 | flags：bit0=保存 Flash，bit1=本地发送 NET_ACK 后重建会话 |
| 8 | 4 | local_ip，仅 28 字节格式提供 |
| 12 | 4 | netmask，仅 28 字节格式提供 |
| 16 | 4 | gateway，仅 28 字节格式提供 |
| 20 | 4 | reconnect_ms，500..60000，仅 28 字节格式提供 |
| 24 | 4 | 保留，28 字节格式置 0 |

8 字节格式只改服务器、端口、ID；28 字节格式还改设备 LAN 配置。
虽然有效扩展字段到偏移 23，当前实现只有 payload_len >= 28 才解析扩展字段，不能发送 24 字节。
采集中 SET_NET 返回 -33，必须先 STOP 并确认停采。
flags=0x03 表示保存并请求重新注册。NET_ACK 仍经原会话提交；随后应等待新端点/新 ID 注册，
不能依赖一定收到这条 UDP 回执。修改远端地址前先确保新服务器已监听。

### 5.5 NET_ACK：36 字节

| payload 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | status，int32 |
| 4 | 4 | server_ip |
| 8 | 2 | server_port |
| 10 | 1 | device_id |
| 11 | 1 | connected，设备已收到合法服务端命令的逻辑状态，不是 TCP ESTABLISHED |
| 12 | 1 | reconnect_pending |
| 13 | 3 | 保留 |
| 16 | 4 | local_ip |
| 20 | 4 | netmask |
| 24 | 4 | gateway |
| 28 | 4 | reconnect_ms |
| 32 | 4 | 保留 |

当前实现先生成 NET_ACK，再设置本次重连标志，因此 SET_NET 成功回执的 reconnect_pending 可能仍为 0。
应根据所发 flags 和后续注册判断，不能等待它必须变成 1。

### 5.6 当前错误码

| 命令/回执 | status | 含义 |
| --- | --- | --- |
| 通用 | 0 | 当前操作成功/状态查询成功，不代表所有 DATA 已送达 |
| SET_PARAM | -10 / -11 | payload 太短 / 参数非法 |
| SET_PARAM | -12 / -13 / -14 | ADC 配置失败 / Flash 保存失败 / 前端设置失败 |
| START | -20 / -21 | ADC 配置失败 / 实际启动采样失败 |
| START | -22 / -23 | 上次 STOP 尚未处理完 / 前端设置失败 |
| SET_NET | -30 / -31 / -32 / -33 | 长度不足 / 参数非法 / Flash 保存失败 / 仍在采集 |
| 未知命令 | -99 | 类型不支持 |

不要假定失败会回滚所有已执行步骤。配置失败后应 GET_PARAM/GET_NET 查询实际状态。

## 6. DATA：可复用格式，必须修改包长假设

### 6.1 DATA meta 与样本

| payload 偏移 | 长度 | 字段 | 说明 |
| ---: | ---: | --- | --- |
| 0 | 4 | fs_hz | 实际采样率，FFT/时间轴使用本值 |
| 4 | 1 | bits | 16 或 24 |
| 5 | 1 | range | 兼容字段 |
| 6 | 1 | bytes_per_sample | 16 位为 2，24 位为 3 |
| 7 | 1 | reserved | 0 |
| 8 | 4 | first_sample_seq | 本帧第一个样点序号，uint32 |
| 12 | 2 | sample_count | 本帧样点数 |
| 14 | 2 | data_bytes | 原始样本区字节数 |
| 16 | data_bytes | samples | 有符号二进制补码，单个样本高字节在前 |

必须验证：data_bytes == sample_count * bytes_per_sample，payload_len == 16 + data_bytes，
bits 与 bytes_per_sample 一致。24 位解码为有符号整数时需要符号扩展，不能直接当 uint24。

```text
24 位样本：00 00 01 -> +1；FF FF FF -> -1；80 00 00 -> -8388608
16 位样本：00 01 -> +1；FF FF -> -1；80 00 -> -32768
```

单通道，不含逐样点 ADC STATUS 字节。当前固件在打包前已启用 100 Hz 软件去直流，
这是已有采样处理而非 UDP 新增功能；帧内没有过滤开关字段，不应当作绝对直流测量原码。

### 6.2 UDP 拆分规则

当前 netif MTU=1280，单报 UDP payload <=1252；每个 DATA 自带完整帧头、meta 和 CRC。
内部仍每块 790 点，只是在传输时拆分：

| 位数 | 完整内部块的典型拆分 | 对应完整应用帧长度 |
| --- | --- | --- |
| 24 位 | 408 点 + 382 点 | 1252 字节 + 1174 字节 |
| 16 位 | 612 点 + 178 点 | 1252 字节 + 384 字节 |

停止时还可能有更短 DATA。以上是当前典型值，不是上位机可写死的包数/点数。
无需先拼回 790 点再解码，每个 DATA 可以独立解析；按样点序号连续排列即可。

### 6.3 采样档位与实际速率

SET_PARAM、ACK、REGISTER 中 fs_hz 是配置档位；DATA meta 的 fs_hz 是实际值。
当前 25 MHz 外部时钟对应如下整数报告值：

| 配置档位 | DATA 实际 fs_hz |
| ---: | ---: |
| 400000 | 390625 |
| 200000 | 195313 |
| 100000 | 97656 |
| 50000 | 48828 |
| 25000 | 24414 |

设置命令仍传左列，不能将右列作为 SET_PARAM 档位发送。
频谱、采样时长、WAV/文件元数据按 DATA 右列更新；不要用网络接收速度代替采样率。

## 7. 乱序、重复、缺口和计数回绕

区分两种序号：

- 帧头 seq：设备所有发出帧共用一个 uint32 序列，包括 REGISTER、ACK、NET_ACK、DATA。
  DATA 相邻两帧的 seq 差值大于 1，可能仅因中间发送了控制帧，不能直接等同丢样。
- first_sample_seq：采样点序号，配合 sample_count 检查数据连续性；只对同一采集轮次比较。

建议每设备建立有界重排窗口，例如 64 帧/20 ms；这是配套 Bridge 的策略示例，不是协议保证。
缺口到期后记录损失，继续处理后续有效帧；旧帧/重复帧丢弃并单独统计。
队列满不能无限增长，应报警和明确记录接收侧丢弃。

在完成重排、且确认同一采集轮次后：

```text
expected_next = (last_first_sample_seq + last_sample_count) & 0xFFFFFFFF
delta = (new_first_sample_seq - expected_next) & 0xFFFFFFFF
delta == 0：连续
0 < delta < 0x80000000：前向缺口，缺失 delta 个样点
delta >= 0x80000000：迟到/重复/采集轮次变化候选，不能当成巨大丢包数
```

uint32 会自然回绕，不能用普通有符号大小比较判断永久离线。
每次实际 START、自动续采、重新配置及会话清理都会涉及样点计数清零；帧头 seq 不一定一起清零。
设备未提供明确 session_id/start_id/时间戳字段，应用必须结合控制流程、注册来源变化、
ACK 状态与序列回退建立自己的采集轮次。存在无法无歧义识别所有迟到包的限制。

不要把跨重连或重启的数据直接当连续波形；文件建议分段记录轮次、实际 fs、缺口和端点变化。
FFT 应选择连续数据段，不要把缺口前后硬拼成等间隔无缺样记录；不允许为掩盖丢样而静默补造样本。
板端 overrun、本地 UDP 发送错误、服务器 CRC 错误、序列缺口、接收队列溢出应分别统计。

## 8. 控制 ACK、超时及重试限制

### 8.1 应用 ACK 不等于 TCP ACK

0x10 ACK / 0x22 NET_ACK 是业务回执；UDP 中不存在 TCP 的确认回调。
设备不回显请求的 type 或 seq，响应帧头 seq 是设备自己的发送序号。
定时 GET_PARAM、初始注册后的主动 ACK、START/STOP/SET_PARAM 都可能产生相同类型的 ACK。
不能使用“响应 seq 等于请求 seq”或“收到第一条 status=0 就认定最近命令成功”。

### 8.2 上位机建议控制流程

1. 每台设备同时最多一个业务控制操作，保活和业务操作共享串行发送调度。
2. 在业务命令前后适当错开 GET_PARAM，避免制造不必要的 ACK 混淆；一次暂停不得接近 30 秒。
3. 设置超时可先取 2~5 秒并按链路调整，这是服务器策略，不是固件承诺的响应期限。
4. 超时后先查询实际状态，再决定是否有限重试；给用户显示“结果待确认”，不要假装失败必未执行。
5. SET_PARAM/带保存位 SET_NET 会写 Flash，不做无限重发。
6. START 成功 ACK 可能 running=0，因为实际启动在 ACK 本地提交之后；观察 DATA 或后续查询确认运行。
7. 运行中重复 START 当前只回执，不重启，但协议没有通用请求去重，也没有 exactly-once 保证。
8. STOP 后等待 running=0，并留有限重排/排空窗口；收到 STOP ACK 后仍可能收到迟到 DATA。

网络可能丢失控制命令、回执或使旧命令迟到。现有协议无法严格关联事务，不能保证旧 START 不会
晚于 STOP 到达。若业务要求严格命令顺序、无歧义确认和安全停采，必须另行升级带请求 ID、
会话 ID、去重/应答规则的协议或保留可靠控制通道，不能声称本版本已经具备。
安全相关停机不能仅依赖这套无认证 UDP 控制。

## 9. 开发参考与联调向量

### 9.1 可复用的编解码辅助函数

以下是协议说明用的 Python 参考片段，不含服务器、设备表、保活、重排和控制调度；
没有自动 START 或写 Flash 操作。逐位 CRC 便于核对，高吞吐生产实现建议查表优化。

```python
import struct

def crc16(data):
    value = 0xFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value

def make_frame(kind, payload=b'', seq=0):
    body = struct.pack('<HBBHI', 0xA55A, 1, kind, len(payload), seq & 0xFFFFFFFF) + payload
    return body + struct.pack('<H', crc16(body))

def parse_frame(packet):
    if not 12 <= len(packet) <= 1252:
        raise ValueError('invalid frame length')
    magic, version, kind, size, seq = struct.unpack_from('<HBBHI', packet)
    if magic != 0xA55A or version != 1 or len(packet) != size + 12:
        raise ValueError('invalid frame header')
    if crc16(packet[:-2]) != struct.unpack_from('<H', packet, len(packet) - 2)[0]:
        raise ValueError('CRC mismatch')
    return kind, seq, packet[10:-2]

def decode_data(payload):
    if len(payload) < 16:
        raise ValueError('short DATA meta')
    fs, bits, range_value, bps, reserved, first, count, size = struct.unpack_from('<IBBBBIHH', payload)
    if fs == 0 or count == 0 or (bits, bps) not in ((16, 2), (24, 3)):
        raise ValueError('invalid sample format')
    if size != count * bps or len(payload) != 16 + size:
        raise ValueError('invalid DATA length')
    raw = payload[16:]
    samples = [int.from_bytes(raw[i:i+bps], 'big', signed=True) for i in range(0, size, bps)]
    return fs, bits, range_value, first, samples
```

UDP 服务端收发流程伪代码：

```text
bind UDP socket 到正确的本地源 IP:9000
持续 recvfrom（或异步接收），每报独立验长和 CRC
    REGISTER -> 校验 ID，登记/更新源端点，安排 GET_PARAM
    已登记源的 ACK/NET_ACK -> 更新状态和待确认业务结果
    已登记源的 DATA -> 每设备重排、去重、缺口统计、解码、保存/绘图
定时器：每个在线设备约每 5 秒从同一个 socket 发 GET_PARAM 到登记端点
定时器：接收超时标离线；新 REGISTER 到来重新确认通信
```

### 9.2 可用于抓包比对的完整命令帧

以下是协议构造的命令测试向量，不是实测采样数据。帧尾已包含正确 CRC：

```text
GET_PARAM，seq=1：
5A A5 01 02 00 00 01 00 00 00 77 C1

START，seq=2：
5A A5 01 03 00 00 02 00 00 00 67 45

STOP，seq=3：
5A A5 01 04 00 00 03 00 00 00 10 79

SET_PARAM，400k 档、24 位、range=0，seq=4（会停采并写 Flash）：
5A A5 01 01 08 00 04 00 00 00 80 1A 06 00 18 00 00 00 65 AC

GET_NET，seq=5：
5A A5 01 21 00 00 05 00 00 00 64 33
```

### 9.3 配套 Bridge 兼容方式

若暂时不重写已有 GUI，可以运行配套 UDP Bridge：设备到 Bridge 为 UDP 9000，
Bridge 到旧 GUI 为本机 TCP 9001，旧 GUI 的字节流解析可继续使用。
这不表示设备仍支持 TCP 9000，也不赋予设备 UDP 数据可靠性。
使用 Bridge 时不要再启动另一个服务占用同一 UDP 9000。

本机公网别名联调示例：

```powershell
py tools/adc_udp_bridge.py --bind-ip 49.239.193.149 --device-id 3 --device-port 9000 --gui-port 9001
```

纯 UDP 自研服务直接实现本文即可，不需要 TCP 9001。可参考交付源码：
`tools/adc_udp_bridge.py`、`device/adc_udp_protocol.c` 和 `device/adc_tcp_server.c`。
后者为兼容构建保留旧文件名，内部实际是 UDP。

## 10. 对接验收清单

- 已放行 UDP 而不是只放行 TCP，服务器回包源 IP/端口与设备配置匹配。
- 收到 REGISTER 后可 GET_PARAM/GET_NET，能从回执确认双向通信。
- 空闲超过 60 秒不误断链，保活不依赖点击采集按钮。
- 16/24 位都能解析，长短 DATA、停止部分块均正常，不硬编码 790 点或固定报文长度。
- FFT/时间轴使用 DATA meta 实际采样率，样本使用大端有符号解码。
- 乱序/重复/丢包被正确记录；缺口不静默补造；32 位回绕不形成巨大丢样数。
- START/STOP 状态核验不依赖响应 seq 等于请求 seq，能处理 ACK 丢失及延迟。
- 网络或服务器中断超过 30 秒，设备停采重注册；恢复后待机/续采符合中断前业务状态。
- 重注册换端口可更新设备表，周期 REGISTER 不反复清空波形。
- 多设备同端口不串数据、不串控制，重复 ID 有明确告警策略。
- 接收线程与绘图/磁盘任务解耦，缓存有界，处理不过来时有真实溢出统计。

本地实板最高档 24 位短测约 1.17 MB/s 原始样本吞吐，20 秒窗口未观察到样点缺口，
仅作为局域网性能参考；不是公网/5G 带宽、长期无丢样或 ADC 模拟精度保证。
