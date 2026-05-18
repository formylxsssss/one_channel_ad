#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TCP_PORT              5000U
#define APP_DEFAULT_FS_HZ         400000UL
#define APP_DEFAULT_BITS          24U
#define APP_DEFAULT_RANGE         0U

#define APP_FS_400K               400000UL
#define APP_FS_200K               200000UL
#define APP_FS_100K               100000UL
#define APP_FS_50K                50000UL
#define APP_FS_25K                25000UL

#define APP_BITS_24               24U
#define APP_BITS_16               16U

/* range 对应硬件 CH1_D0~CH1_D3，只允许一个档位有效 */
#define APP_RANGE_D0_1K           0U
#define APP_RANGE_D1_10K          1U
#define APP_RANGE_D2_100K         2U
#define APP_RANGE_D3_1M           3U

/*
 * ADC 数据块大小。
 * 320 点一包：
 *   24bit: DATA帧约 988 字节
 *   16bit: DATA帧约 668 字节
 * 这样小于常见 TCP_SND_BUF 单次可写空间，便于实时发送。
 */
#define APP_ADC_BLOCK_SAMPLES     320U
#define APP_ADC_BLOCK_COUNT       64U

/*
 * 连续 SPI DMA 读取 ADS127L11：
 *   - ADS127L11 开启 STATUS 字节；
 *   - 24bit 输出帧 = STATUS + 3字节数据 = 4字节；
 *   - 16bit 输出帧 = STATUS + 2字节数据 = 3字节；
 *   - SPI4 以 21MHz 连续时钟读取；
 *   - 只保留 STATUS.DRDY=1 的新样点，丢弃重复旧样点。
 *
 * 每半缓冲 512 个 ADC 输出帧：
 *   24bit: 半缓冲 2048 字节，约 0.78ms 产生一次 DMA 半完成中断；
 *   16bit: 半缓冲 1536 字节，约 0.58ms 产生一次 DMA 半完成中断。
 */
#define APP_ADC_CONT_STATUS_ENABLE        1U
#define APP_ADC_CONT_MAX_FRAME_BYTES      4U
#define APP_ADC_CONT_DMA_FRAMES_PER_HALF  512U
#define APP_ADC_CONT_DMA_TOTAL_FRAMES     (APP_ADC_CONT_DMA_FRAMES_PER_HALF * 2U)
#define APP_ADC_CONT_DMA_MAX_BYTES        (APP_ADC_CONT_DMA_TOTAL_FRAMES * APP_ADC_CONT_MAX_FRAME_BYTES)

#define APP_PARAM_MAGIC           0xADC12711UL
#define APP_PARAM_VERSION         0x00010003UL

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
#define APP_STREAM_FLAG_OVERRUN_STOPPED   0x00000001UL  /* 历史命名保留：现在表示缓存满导致丢样 */
#define APP_STREAM_FLAG_TCP_BACKPRESSURE  0x00000002UL
#define APP_STREAM_FLAG_SPI_TIMEOUT       0x00000004UL
#define APP_STREAM_FLAG_ISR_STOPPED       0x00000008UL
#define APP_STREAM_FLAG_SPI_DMA_BUSY      0x00000010UL
#define APP_STREAM_FLAG_SPI_DMA_ERROR     0x00000020UL
#define APP_STREAM_FLAG_CONT_DMA_ERROR    0x00000040UL
#define APP_STREAM_FLAG_ADC_STATUS_ERR    0x00000080UL

/* ADS127L11 STATUS 字节 bit0：DRDY=1 表示当前读到的是新转换数据。 */
#define APP_ADS_STATUS_DRDY_BIT           0x01U

/* 量产阶段可关闭每秒统计 log，避免 RTT 对高速传输造成干扰。 */
#ifndef ADC_TCP_TRACE_ENABLE
#define ADC_TCP_TRACE_ENABLE              1U
#endif

#ifndef ADC_TCP_DATA_TRACE_ENABLE
#define ADC_TCP_DATA_TRACE_ENABLE         0U
#endif

typedef struct
{
    uint32_t fs_hz;
    uint8_t  bits;
    uint8_t  range;
    uint8_t  reserved0;
    uint8_t  reserved1;
} AppConfig_t;

extern AppConfig_t g_app_cfg;

int AppConfig_IsValid(const AppConfig_t *cfg);
AppConfig_t AppConfig_Default(void);
uint8_t AppConfig_BytesPerSample(const AppConfig_t *cfg);
uint8_t AppConfig_AdcFilterCode(uint32_t fs_hz);

#ifdef __cplusplus
}
#endif

#endif
