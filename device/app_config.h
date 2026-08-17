#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TCP_PORT              5000U
#define APP_DEFAULT_FS_HZ         400000UL
#define APP_DEFAULT_BITS          24U

/*
 * 本版把原来的 range 字段改为“前端增益档位”。
 * 为了不改动协议结构、Flash 结构、TCP DATA meta 结构，字段名仍保留为 range。
 * 实际含义如下：
 *   range = 0 -> 0 dB
 *   range = 1 -> 20 dB
 *   range = 2 -> 40 dB
 *   range = 3 -> 保留，正常不允许使用
 */
#define APP_GAIN_0DB              0U
#define APP_GAIN_20DB             1U
#define APP_GAIN_40DB             2U
#define APP_GAIN_RESERVED         3U
#define APP_DEFAULT_RANGE         APP_GAIN_0DB
#define APP_DEFAULT_GAIN          APP_GAIN_0DB

/* 兼容旧代码/旧上位机里的 range 命名，避免其它文件大面积改名。 */
#define APP_RANGE_GAIN_0DB        APP_GAIN_0DB
#define APP_RANGE_GAIN_20DB       APP_GAIN_20DB
#define APP_RANGE_GAIN_40DB       APP_GAIN_40DB
#define APP_RANGE_RESERVED        APP_GAIN_RESERVED

/* 旧版本宏名兼容：旧的 D0/D1/D2/D3 现在分别映射到 0/20/40dB/保留。 */
#define APP_RANGE_D0_1K           APP_GAIN_0DB
#define APP_RANGE_D1_10K          APP_GAIN_20DB
#define APP_RANGE_D2_100K         APP_GAIN_40DB
#define APP_RANGE_D3_1M           APP_GAIN_RESERVED

#define APP_FS_400K               400000UL
#define APP_FS_200K               200000UL
#define APP_FS_100K               100000UL
#define APP_FS_50K                50000UL
#define APP_FS_25K                25000UL

#define APP_BITS_24               24U
#define APP_BITS_16               16U

/*
 * TCP 数据块大小。
 * 954 点一包：
 *   24bit DATA 帧 = 10 + 16 + 954*3 + 2 = 2890 字节，约 2 个 TCP_MSS。
 *   16bit DATA 帧 = 10 + 16 + 954*2 + 2 = 1936 字节。
 */
#define APP_ADC_BLOCK_SAMPLES     954U
#define APP_ADC_BLOCK_COUNT       32U

/*
 * DRDY 同步启动 + SPI4 连续 DMA。
 * 强制打开 ADS127L11 STATUS header，连续 DMA 只作为高速轮询；
 * 真正入队发送的数据只接受 STATUS.DRDY(bit0)=1 的新转换帧。
 */
#define APP_ADC_CONT_STATUS_ENABLE        1U
#define APP_ADC_CONT_REQUIRE_STATUS_DRDY  1U
#define APP_ADC_CONT_MAX_FRAME_BYTES      4U
#define APP_ADC_CONT_DMA_FRAMES_PER_HALF  4096U
#define APP_ADC_CONT_DMA_TOTAL_FRAMES     (APP_ADC_CONT_DMA_FRAMES_PER_HALF * 2U)
#define APP_ADC_CONT_DMA_MAX_BYTES        (APP_ADC_CONT_DMA_TOTAL_FRAMES * APP_ADC_CONT_MAX_FRAME_BYTES)

#define APP_PARAM_MAGIC           0xADC12711UL

/*
 * 参数版本号必须升级一次：
 * 旧 Flash 里保存的是“阻抗/量程档位”，现在 range 字段变成“增益档位”。
 * 升级版本号后，首次上电会使用默认值 400k / 24bit / 0dB，避免误用旧参数。
 */
#define APP_PARAM_VERSION         0x00010009UL

/* TCP 协议固定字段 */
#define APP_PROTO_MAGIC           0xA55AU
#define APP_PROTO_VER             0x01U
#define APP_FRAME_HDR_LEN         10U
#define APP_FRAME_CRC_LEN         2U
#define APP_DATA_META_LEN         16U
#define APP_DATA_FRAME_MAX_LEN    (APP_FRAME_HDR_LEN + APP_DATA_META_LEN + (APP_ADC_BLOCK_SAMPLES * 3U) + APP_FRAME_CRC_LEN)
#define APP_ACK_FRAME_MAX_LEN     64U

#define APP_TYPE_SET_PARAM        0x01U
#define APP_TYPE_GET_PARAM        0x02U
#define APP_TYPE_START            0x03U
#define APP_TYPE_STOP             0x04U
#define APP_TYPE_ACK              0x10U
#define APP_TYPE_DATA             0x81U

/* 流状态位 */
#define APP_STREAM_FLAG_OVERRUN_STOPPED   0x00000001UL
#define APP_STREAM_FLAG_TCP_BACKPRESSURE  0x00000002UL
#define APP_STREAM_FLAG_SPI_TIMEOUT       0x00000004UL
#define APP_STREAM_FLAG_ISR_STOPPED       0x00000008UL
#define APP_STREAM_FLAG_SPI_DMA_BUSY      0x00000010UL
#define APP_STREAM_FLAG_SPI_DMA_ERROR     0x00000020UL
#define APP_STREAM_FLAG_CONT_DMA_ERROR    0x00000040UL
#define APP_STREAM_FLAG_ADC_STATUS_ERR    0x00000080UL
#define APP_STREAM_FLAG_WAIT_FIRST_DRDY   0x00000100UL
#define APP_STREAM_FLAG_DMA_STARTED       0x00000200UL
#define APP_STREAM_FLAG_STATUS_NO_NEW     0x00000400UL
#define APP_STREAM_FLAG_FRAME_ALIGN_WARN  0x00000800UL
#define APP_STREAM_FLAG_DMA_PENDING_DROP  0x00001000UL

/* ADS127L11 STATUS 字节 bit0：DRDY=1 表示本帧为新转换数据。 */
#define APP_ADS_STATUS_DRDY_BIT           0x01U

#ifndef APP_ADC_RAW_DUMP_ENABLE
#define APP_ADC_RAW_DUMP_ENABLE           0U
#endif
#ifndef APP_ADC_RAW_DUMP_FRAMES_ON_START
#define APP_ADC_RAW_DUMP_FRAMES_ON_START  128U
#endif
#ifndef APP_ADC_RAW_DUMP_PRINTS_PER_TASK
#define APP_ADC_RAW_DUMP_PRINTS_PER_TASK  4U
#endif

#ifndef APP_TCP_SEND_BURST_MAX
#define APP_TCP_SEND_BURST_MAX            6U
#endif

#ifndef ADC_TCP_TRACE_ENABLE
#define ADC_TCP_TRACE_ENABLE              1U
#endif

#ifndef ADC_TCP_DATA_TRACE_ENABLE
#define ADC_TCP_DATA_TRACE_ENABLE         0U
#endif

#ifndef ADC_STREAM_STAT_LOG_ENABLE
#define ADC_STREAM_STAT_LOG_ENABLE        1U
#endif

typedef struct
{
    uint32_t fs_hz;
    uint8_t  bits;
    uint8_t  range;     /* 现在表示增益档位：0=0dB, 1=20dB, 2=40dB */
    uint8_t  reserved0;
    uint8_t  reserved1;
} AppConfig_t;

extern AppConfig_t g_app_cfg;

int AppConfig_IsValid(const AppConfig_t *cfg);
AppConfig_t AppConfig_Default(void);
uint8_t AppConfig_BytesPerSample(const AppConfig_t *cfg);
uint8_t AppConfig_AdcFilterCode(uint32_t fs_hz);
uint8_t AppConfig_RangeToGainDb(uint8_t range);
uint16_t AppConfig_RangeToGainX(uint8_t range);
const char *AppConfig_RangeName(uint8_t range);

#ifdef __cplusplus
}
#endif

#endif
