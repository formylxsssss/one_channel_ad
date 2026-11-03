# TCP Client 分支的软件去直流偏置

代码位置：`device/adc_stream.c`、`device/adc_dc_filter.h`。
当前本地 `master` 提交中未找到去偏置实现；本次按此前确认的 100Hz 要求重新加入，未复制服务端或网络重连代码。

## 开启、关闭

`device/app_config.h` 中默认配置：

```c
#define APP_ADC_DC_REMOVE_ENABLE  1U
#define APP_ADC_DC_CUTOFF_HZ      100U
```

- 默认开启，START 后自动处理，不需要上位机发新命令。
- 将 `APP_ADC_DC_REMOVE_ENABLE` 改为 `0U` 后重新编译、烧录，即恢复发送原始 ADC 样点。
- 截止频率通过 `APP_ADC_DC_CUTOFF_HZ` 修改；不是 Flash 参数，也不是现有 SET_PARAM 字段。
- 本次没有改变协议、设备 ID、采样率、采样位宽、模拟增益或 TCP 重连逻辑。

## 处理方式

只处理 STATUS.DRDY=1 的新转换帧。在写入 DATA 样点区之前，先把大端二补码解码为有符号数，再执行一阶高通并限幅，最后按原来的 16/24 位大端格式打包。帧头和 CRC 仍由原发送路径生成。

滤波器使用缓慢跟踪的均值估计：

```text
alpha = 2*pi*fc / (fs + 2*pi*fc)
mean[n] = mean[n-1] + alpha * (x[n] - mean[n-1])
y[n] = x[n] - mean[n]
```

系数在 START 时按采样率计算。逐样点使用 Q16 状态、Q20 系数和 64 位整数中间量，不使用逐样点浮点运算或动态分配内存。

每次 START 重新初始化，以第一个有效样点作为均值初值；状态跨 DMA 分块和 TCP 数据包保持，发送背压造成丢样时仍继续更新。首样点输出为零，开始采集及偏置突变后会有短暂的滤波过渡过程。

100Hz 是近似截止频率，并非把低于 100Hz 的信号全部删除。100Hz 正弦幅度约保留 70%，1kHz 约保留 98%～99.4%（随采样率略变）；低频有效信号也会衰减。ADC 或模拟前端已经削顶的数据无法通过此功能恢复。

## 验证

不连接开发板，直接执行编译后的 `adc_stream.c` Thumb 指令，硬件调用由桩函数替代：

```powershell
py -m pip install unicorn
py tests/test_adc_dc_remove.py
```

需要 `arm-none-eabi-gcc`、`arm-none-eabi-nm` 在 PATH 中，测试产物位于已忽略的 `build/dc-tests`。

覆盖五档采样率、16/24 位正负直流、满量程跳变限幅、跨包连续性、START 重置、跳过旧 STATUS 帧、背压丢样期间的状态更新、100Hz/1kHz 频响以及关闭功能后原始字节保持不变。

仿真不能代替板上验证，400kSPS 下的实际 CPU 负载、丢样和网络吞吐需要烧录后测试。
