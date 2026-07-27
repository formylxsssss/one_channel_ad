#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * STM32F427VGT6 + LAN8720A + ADS127L11
 *
 * 说明：
 * 1. APP_TCP_PORT 保留给旧代码兼容；本版 adc_tcp_server.c 内部已经改成 TCP Client，
 *    不再监听 APP_TCP_PORT，而是主动连接 g_app_net_cfg.server_ip:server_port。
 * 2. 服务器公网 IP / 端口 / 设备 ID 在 app_net_config.h/.c 中管理，可通过上位机 SET_NET 修改并保存。
 */
#define APP_TCP_PORT              5000U

#define APP_DEFAULT_FS_HZ         400000UL
#define APP_DEFAULT_BITS          24U

/*
 * range 字段现在表示“前端增益档位”：
 *   range = 0 -> 0 dB  -> x1
 *   range = 1 -> 20 dB -> x10
 *   range = 2 -> 40 dB -> x100
 *   range = 3 -> 保留
 *
 * 为了不破坏现有协议、Flash结构、DATA meta结构，字段名仍然叫 range。
 */
#define APP_GAIN_0DB              0U
#define APP_GAIN_20DB             1U
#define APP_GAIN_40DB             2U
#define APP_GAIN_RESERVED         3U
#define APP_DEFAULT_RANGE         APP_GAIN_0DB
#define APP_DEFAULT_GAIN          APP_GAIN_0DB

#define APP_RANGE_GAIN_0DB        APP_GAIN_0DB
#define APP_RANGE_GAIN_20DB       APP_GAIN_20DB
#define APP_RANGE_GAIN_40DB       APP_GAIN_40DB
#define APP_RANGE_RESERVED        APP_GAIN_RESERVED

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
 * TCP DATA block size.
 */
#define APP_ADC_BLOCK_SAMPLES     790U
#define APP_ADC_BLOCK_COUNT       24U

/*
 * Effective DATA frame size for TCP_MSS=1200:
 *   24-bit: 10 + 16 + 790*3 + 2 = 2398 bytes, within 2 TCP segments.
 *   16-bit: 10 + 16 + 790*2 + 2 = 1608 bytes.
 * DATA uses TCP_WRITE_FLAG_COPY, so ADC blocks are released after tcp_write
 * copies them. 24 blocks keep about 47 ms of local ADC buffering and leave
 * more SRAM for the LwIP TCP send heap.
 */

/*
 * DRDY同步启动 + SPI4连续DMA。
 * 强制打开 ADS127L11 STATUS header，连续DMA只作为高速轮询；
 * 真正入队发送的数据只接受 STATUS.DRDY(bit0)=1 的新转换帧。
 */
#define APP_ADC_CONT_STATUS_ENABLE        1U
#define APP_ADC_CONT_REQUIRE_STATUS_DRDY  1U
#define APP_ADC_CONT_MAX_FRAME_BYTES      4U
#define APP_ADC_CONT_DMA_FRAMES_PER_HALF  4096U
#define APP_ADC_CONT_DMA_TOTAL_FRAMES     (APP_ADC_CONT_DMA_FRAMES_PER_HALF * 2U)
#define APP_ADC_CONT_DMA_MAX_BYTES        (APP_ADC_CONT_DMA_TOTAL_FRAMES * APP_ADC_CONT_MAX_FRAME_BYTES)

/*
 * Flash参数。
 * 本版把“采样参数”和“5G服务器连接参数”放到同一个Flash记录里，因此版本号升级。
 * 默认使用 STM32F427VG 1MB Flash 的 Sector 11，地址 0x080E0000。
 * 如果你的工程原来已经在别的扇区保存参数，只需要在编译宏中覆盖
 * APP_PARAM_FLASH_SECTOR / APP_PARAM_FLASH_ADDR。
 */
#define APP_PARAM_MAGIC           0xADC12711UL
#define APP_PARAM_VERSION         0x0001000AUL

#ifndef APP_PARAM_FLASH_SECTOR
#define APP_PARAM_FLASH_SECTOR    FLASH_SECTOR_11
#endif

#ifndef APP_PARAM_FLASH_ADDR
#define APP_PARAM_FLASH_ADDR      0x080E0000UL
#endif

/* 协议固定字段 */
#define APP_PROTO_MAGIC           0xA55AU
#define APP_PROTO_VER             0x01U
#define APP_FRAME_HDR_LEN         10U
#define APP_FRAME_CRC_LEN         2U
#define APP_DATA_META_LEN         16U
#define APP_DATA_FRAME_MAX_LEN    (APP_FRAME_HDR_LEN + APP_DATA_META_LEN + (APP_ADC_BLOCK_SAMPLES * 3U) + APP_FRAME_CRC_LEN)
#define APP_ACK_FRAME_MAX_LEN     96U

#define APP_TYPE_SET_PARAM        0x01U
#define APP_TYPE_GET_PARAM        0x02U
#define APP_TYPE_START            0x03U
#define APP_TYPE_STOP             0x04U
#define APP_TYPE_ACK              0x10U
#define APP_TYPE_DATA             0x81U

/*
 * 5G服务器连接参数扩展命令。
 * 上位机 -> 下位机：
 *   APP_TYPE_GET_NET: payload_len=0
 *   APP_TYPE_SET_NET: 兼容两种长度：
 *                     8字节：payload[0..3]=server_ip；payload[4..5]=server_port小端；
 *                            payload[6]=device_id；payload[7]=flags。
 *                     28字节及以上：在8字节后追加 local_ip/netmask/gateway/reconnect_ms。
 *                     flags bit0=保存到Flash，bit1=ACK发出后立即断开并按新参数重连。
 *
 * 下位机 -> 上位机：
 *   APP_TYPE_NET_ACK: payload[0..3]=int32 status；
 *                     payload[4..7]=server_ip；payload[8..9]=server_port；
 *                     payload[10]=device_id；payload[11]=connected；
 *                     payload[12]=reconnect_pending；payload[13..15]=保留；
 *                     payload[16..19]=local_ip；payload[20..23]=netmask；
 *                     payload[24..27]=gateway；payload[28..31]=reconnect_ms；
 *                     payload[32..35]=保留。
 *
 * 下位机 -> 服务器：
 *   APP_TYPE_REGISTER: 设备主动连接服务器成功后发送一次注册帧，便于服务器识别设备。
 */
#define APP_TYPE_SET_NET          0x20U
#define APP_TYPE_GET_NET          0x21U
#define APP_TYPE_NET_ACK          0x22U
#define APP_TYPE_REGISTER         0x30U

#define APP_NET_SET_FLAG_SAVE        0x01U
#define APP_NET_SET_FLAG_RECONNECT   0x02U

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
    uint8_t  range;
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
