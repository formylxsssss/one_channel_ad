#include "adc_tcp_server.h"
#include "app_config.h"
#include "flash_param.h"
#include "app_net_config.h"
#include "ads127l11.h"
#include "adc_stream.h"
#include "range_ctrl.h"
#include "main.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "SEGGER_RTT.h"
#include <string.h>

#define TXQ_LEN         (APP_ADC_BLOCK_COUNT + 8U)
#define TXQ_TYPE_CTRL   0U
#define TXQ_TYPE_DATA   1U

#ifndef ADC_TCP_TRACE_ENABLE
#define ADC_TCP_TRACE_ENABLE    1U
#endif

/* 高速采集时不要打印每个 DATA 包，否则 RTT 会严重拖慢 TCP 发送。 */
#ifndef ADC_TCP_DATA_TRACE_ENABLE
#define ADC_TCP_DATA_TRACE_ENABLE 0U
#endif

typedef struct
{
    uint16_t len;
    uint8_t type;
    uint8_t reserved;
    AdcStreamBlock_t *blk;
} TxQueueItem_t;

static struct tcp_pcb *s_connect_pcb = 0;
static struct tcp_pcb *s_client = 0;
static uint8_t s_rxbuf[256];
static uint16_t s_rxlen = 0;
static uint32_t s_frame_seq = 0;
static uint8_t s_ackbuf[APP_ACK_FRAME_MAX_LEN];

static TxQueueItem_t s_txq[TXQ_LEN];
static uint8_t s_txq_head = 0;
static uint8_t s_txq_tail = 0;
static uint16_t s_txq_count = 0;

static uint32_t s_unacked_total_bytes = 0;
static uint32_t s_unacked_data_bytes = 0;
static uint32_t s_tcp_err_mem = 0;
static uint32_t s_tcp_sndq_full = 0;
static uint32_t s_tcp_sent_frames = 0;
static uint32_t s_tcp_acked_data_frames = 0;
static uint32_t s_tcp_data_bytes_written = 0;
static uint32_t s_tcp_write_err_count = 0;
static uint32_t s_tcp_no_sndbuf_count = 0;
static uint32_t s_tcp_no_seg_count = 0;
static uint32_t s_tcp_drop_ready_count = 0;
static int32_t s_pending_stop_ack = 0;
static uint8_t s_connecting = 0;
static uint8_t s_reconnect_after_ack = 0;
static uint32_t s_next_connect_tick = 0;
static uint32_t s_connect_attempt_count = 0;

/*
 * START 防卡死修改：
 * 1. 收到 START 后先配置 ADC 并发送 ACK；
 * 2. 等 START ACK 被 TCP 确认后，再在 AdcTcpServer_Task() 中真正启动采集。
 */
static uint8_t s_start_wait_ack = 0;
static uint8_t s_start_do_now = 0;
static uint8_t s_stream_requested = 0;
static uint8_t s_resume_stream_after_reconnect = 0;

/*
 * 采集无数据排查用统计。
 * 不打印每个采样点，只每 1 秒打印一次状态，避免 RTT 影响高速采集。
 */
static uint32_t s_stream_start_tick = 0;
static uint32_t s_last_stream_stat_tick = 0;
static uint32_t s_last_stream_seq = 0;
static uint32_t s_last_stream_irq = 0;
static uint8_t  s_no_drdy_warned = 0;
static uint8_t  s_no_data_warned = 0;
static uint8_t  s_stream_debug_active = 0;
static uint8_t  s_stream_stop_reported = 0;
static uint32_t s_last_isr_stop_count = 0;
static uint8_t  s_data_frame_too_large_warned = 0;
static uint32_t s_tx_block_wait_count = 0;
static uint32_t s_last_tx_frames = 0;
static uint32_t s_last_acked_frames = 0;
static uint32_t s_last_tcp_bytes = 0;
static uint32_t s_last_memerr = 0;
static uint32_t s_last_overrun = 0;
static uint32_t s_last_drop_ready = 0;

#if (ADC_TCP_TRACE_ENABLE != 0U)
#define TCP_LOG_INFO(...)   do { SEGGER_RTT_printf(0, "[TCP] "); SEGGER_RTT_printf(0, __VA_ARGS__); SEGGER_RTT_printf(0, "\r\n"); } while (0)
#define TCP_LOG_OK(...)     do { SEGGER_RTT_printf(0, "[TCP OK] "); SEGGER_RTT_printf(0, __VA_ARGS__); SEGGER_RTT_printf(0, "\r\n"); } while (0)
#define TCP_LOG_ERR(...)    do { SEGGER_RTT_printf(0, "[TCP FAIL] "); SEGGER_RTT_printf(0, __VA_ARGS__); SEGGER_RTT_printf(0, "\r\n"); } while (0)
#else
#define TCP_LOG_INFO(...)   do { } while (0)
#define TCP_LOG_OK(...)     do { } while (0)
#define TCP_LOG_ERR(...)    do { } while (0)
#endif

static const char *cmd_name(uint8_t type)
{
    switch (type)
    {
        case APP_TYPE_SET_PARAM:
            return "SET_PARAM";

        case APP_TYPE_GET_PARAM:
            return "GET_PARAM";

        case APP_TYPE_START:
            return "START";

        case APP_TYPE_STOP:
            return "STOP";

        case APP_TYPE_ACK:
            return "ACK";

        case APP_TYPE_DATA:
            return "DATA";

        case APP_TYPE_SET_NET:
            return "SET_NET";

        case APP_TYPE_GET_NET:
            return "GET_NET";

        case APP_TYPE_NET_ACK:
            return "NET_ACK";

        case APP_TYPE_REGISTER:
            return "REGISTER";

        default:
            return "UNKNOWN";
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t txq_next(uint8_t i)
{
    i++;
    if (i >= TXQ_LEN)
    {
        i = 0;
    }
    return i;
}

static uint8_t txq_prev(uint8_t i)
{
    if (i == 0U)
    {
        i = TXQ_LEN;
    }

    return (uint8_t)(i - 1U);
}

static void txq_reset(void)
{
    memset(s_txq, 0, sizeof(s_txq));
    s_txq_head = 0;
    s_txq_tail = 0;
    s_txq_count = 0;
    s_unacked_total_bytes = 0;
    s_unacked_data_bytes = 0;
}

static int txq_push(uint8_t type, uint16_t len, AdcStreamBlock_t *blk)
{
    if (s_txq_count >= TXQ_LEN)
    {
        return -1;
    }

    s_txq[s_txq_tail].type = type;
    s_txq[s_txq_tail].len = len;
    s_txq[s_txq_tail].blk = blk;
    s_txq_tail = txq_next(s_txq_tail);
    s_txq_count++;

    s_unacked_total_bytes += len;
    if (type == TXQ_TYPE_DATA)
    {
        s_unacked_data_bytes += len;
    }

    return 0;
}

static void txq_pop_last(void)
{
    TxQueueItem_t *it;

    if (s_txq_count == 0U)
    {
        return;
    }

    s_txq_tail = txq_prev(s_txq_tail);
    it = &s_txq[s_txq_tail];

    if (s_unacked_total_bytes >= it->len)
    {
        s_unacked_total_bytes -= it->len;
    }
    else
    {
        s_unacked_total_bytes = 0;
    }

    if (it->type == TXQ_TYPE_DATA)
    {
        if (s_unacked_data_bytes >= it->len)
        {
            s_unacked_data_bytes -= it->len;
        }
        else
        {
            s_unacked_data_bytes = 0;
        }
    }

    memset(it, 0, sizeof(*it));
    s_txq_count--;
}

static uint16_t tcp_sndq_len_now(void)
{
    if (s_client == 0)
    {
        return 0U;
    }
#ifdef tcp_sndqueuelen
    return tcp_sndqueuelen(s_client);
#else
    return s_client->snd_queuelen;
#endif
}

static uint16_t tcp_required_segs(uint16_t len)
{
    uint16_t segs = (uint16_t)((len + (uint16_t)TCP_MSS - 1U) / (uint16_t)TCP_MSS);
    if (segs == 0U)
    {
        segs = 1U;
    }
    return segs;
}

static uint16_t crc16_modbus(const uint8_t *data, uint32_t len)
{
    static const uint16_t s_crc_table[256] =
    {
        0x0000U, 0xC0C1U, 0xC181U, 0x0140U, 0xC301U, 0x03C0U, 0x0280U, 0xC241U, 0xC601U, 0x06C0U, 0x0780U, 0xC741U, 0x0500U, 0xC5C1U, 0xC481U, 0x0440U,
        0xCC01U, 0x0CC0U, 0x0D80U, 0xCD41U, 0x0F00U, 0xCFC1U, 0xCE81U, 0x0E40U, 0x0A00U, 0xCAC1U, 0xCB81U, 0x0B40U, 0xC901U, 0x09C0U, 0x0880U, 0xC841U,
        0xD801U, 0x18C0U, 0x1980U, 0xD941U, 0x1B00U, 0xDBC1U, 0xDA81U, 0x1A40U, 0x1E00U, 0xDEC1U, 0xDF81U, 0x1F40U, 0xDD01U, 0x1DC0U, 0x1C80U, 0xDC41U,
        0x1400U, 0xD4C1U, 0xD581U, 0x1540U, 0xD701U, 0x17C0U, 0x1680U, 0xD641U, 0xD201U, 0x12C0U, 0x1380U, 0xD341U, 0x1100U, 0xD1C1U, 0xD081U, 0x1040U,
        0xF001U, 0x30C0U, 0x3180U, 0xF141U, 0x3300U, 0xF3C1U, 0xF281U, 0x3240U, 0x3600U, 0xF6C1U, 0xF781U, 0x3740U, 0xF501U, 0x35C0U, 0x3480U, 0xF441U,
        0x3C00U, 0xFCC1U, 0xFD81U, 0x3D40U, 0xFF01U, 0x3FC0U, 0x3E80U, 0xFE41U, 0xFA01U, 0x3AC0U, 0x3B80U, 0xFB41U, 0x3900U, 0xF9C1U, 0xF881U, 0x3840U,
        0x2800U, 0xE8C1U, 0xE981U, 0x2940U, 0xEB01U, 0x2BC0U, 0x2A80U, 0xEA41U, 0xEE01U, 0x2EC0U, 0x2F80U, 0xEF41U, 0x2D00U, 0xEDC1U, 0xEC81U, 0x2C40U,
        0xE401U, 0x24C0U, 0x2580U, 0xE541U, 0x2700U, 0xE7C1U, 0xE681U, 0x2640U, 0x2200U, 0xE2C1U, 0xE381U, 0x2340U, 0xE101U, 0x21C0U, 0x2080U, 0xE041U,
        0xA001U, 0x60C0U, 0x6180U, 0xA141U, 0x6300U, 0xA3C1U, 0xA281U, 0x6240U, 0x6600U, 0xA6C1U, 0xA781U, 0x6740U, 0xA501U, 0x65C0U, 0x6480U, 0xA441U,
        0x6C00U, 0xACC1U, 0xAD81U, 0x6D40U, 0xAF01U, 0x6FC0U, 0x6E80U, 0xAE41U, 0xAA01U, 0x6AC0U, 0x6B80U, 0xAB41U, 0x6900U, 0xA9C1U, 0xA881U, 0x6840U,
        0x7800U, 0xB8C1U, 0xB981U, 0x7940U, 0xBB01U, 0x7BC0U, 0x7A80U, 0xBA41U, 0xBE01U, 0x7EC0U, 0x7F80U, 0xBF41U, 0x7D00U, 0xBDC1U, 0xBC81U, 0x7C40U,
        0xB401U, 0x74C0U, 0x7580U, 0xB541U, 0x7700U, 0xB7C1U, 0xB681U, 0x7640U, 0x7200U, 0xB2C1U, 0xB381U, 0x7340U, 0xB101U, 0x71C0U, 0x7080U, 0xB041U,
        0x5000U, 0x90C1U, 0x9181U, 0x5140U, 0x9301U, 0x53C0U, 0x5280U, 0x9241U, 0x9601U, 0x56C0U, 0x5780U, 0x9741U, 0x5500U, 0x95C1U, 0x9481U, 0x5440U,
        0x9C01U, 0x5CC0U, 0x5D80U, 0x9D41U, 0x5F00U, 0x9FC1U, 0x9E81U, 0x5E40U, 0x5A00U, 0x9AC1U, 0x9B81U, 0x5B40U, 0x9901U, 0x59C0U, 0x5880U, 0x9841U,
        0x8801U, 0x48C0U, 0x4980U, 0x8941U, 0x4B00U, 0x8BC1U, 0x8A81U, 0x4A40U, 0x4E00U, 0x8EC1U, 0x8F81U, 0x4F40U, 0x8D01U, 0x4DC0U, 0x4C80U, 0x8C41U,
        0x4400U, 0x84C1U, 0x8581U, 0x4540U, 0x8701U, 0x47C0U, 0x4680U, 0x8641U, 0x8201U, 0x42C0U, 0x4380U, 0x8341U, 0x4100U, 0x81C1U, 0x8081U, 0x4040U
    };

    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0; i < len; i++)
    {
        crc = (uint16_t)((crc >> 8) ^ s_crc_table[(crc ^ data[i]) & 0xFFU]);
    }
    return crc;
}

static uint16_t build_small_frame(uint8_t type, uint32_t seq, const uint8_t *payload, uint16_t len, uint8_t *out)
{
    put_u16(&out[0], APP_PROTO_MAGIC);
    out[2] = APP_PROTO_VER;
    out[3] = type;
    put_u16(&out[4], len);
    put_u32(&out[6], seq);

    if ((len != 0U) && (payload != 0))
    {
        memcpy(&out[APP_FRAME_HDR_LEN], payload, len);
    }

    uint16_t crc = crc16_modbus(out, APP_FRAME_HDR_LEN + len);
    put_u16(&out[APP_FRAME_HDR_LEN + len], crc);

    return (uint16_t)(APP_FRAME_HDR_LEN + len + APP_FRAME_CRC_LEN);
}

static err_t send_small_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (s_client == 0)
    {
        return ERR_CONN;
    }

    if ((uint16_t)(APP_FRAME_HDR_LEN + len + APP_FRAME_CRC_LEN) > APP_ACK_FRAME_MAX_LEN)
    {
        return ERR_ARG;
    }

    if (s_txq_count >= TXQ_LEN)
    {
        return ERR_MEM;
    }

    uint16_t frame_len = build_small_frame(type, s_frame_seq++, payload, len, s_ackbuf);

    if (tcp_sndbuf(s_client) < frame_len)
    {
        return ERR_MEM;
    }

    err_t e = tcp_write(s_client, s_ackbuf, frame_len, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK)
    {
        if (txq_push(TXQ_TYPE_CTRL, frame_len, 0) != 0)
        {
            return ERR_MEM;
        }
        (void)tcp_output(s_client);
    }

    return e;
}

static void build_data_frame(AdcStreamBlock_t *blk)
{
    uint8_t *f = blk->frame;
    uint16_t payload_len = (uint16_t)(APP_DATA_META_LEN + blk->data_bytes);

    put_u16(&f[0], APP_PROTO_MAGIC);
    f[2] = APP_PROTO_VER;
    f[3] = APP_TYPE_DATA;
    put_u16(&f[4], payload_len);
    put_u32(&f[6], s_frame_seq++);

    uint8_t *p = &f[APP_FRAME_HDR_LEN];
    put_u32(&p[0], g_app_cfg.fs_hz);
    p[4] = g_app_cfg.bits;
    p[5] = g_app_cfg.range;
    p[6] = blk->bytes_per_sample;
    p[7] = 0;
    put_u32(&p[8], blk->first_sample_seq);
    put_u16(&p[12], blk->sample_count);
    put_u16(&p[14], blk->data_bytes);

    blk->frame_len = (uint16_t)(APP_FRAME_HDR_LEN + payload_len + APP_FRAME_CRC_LEN);
    uint16_t crc = crc16_modbus(f, (uint32_t)(blk->frame_len - APP_FRAME_CRC_LEN));
    put_u16(&f[blk->frame_len - APP_FRAME_CRC_LEN], crc);
}

static err_t send_ack(int32_t status)
{
    uint8_t p[44];

    put_u32(&p[0], (uint32_t)status);
    put_u32(&p[4], g_app_cfg.fs_hz);
    p[8] = g_app_cfg.bits;
    p[9] = g_app_cfg.range;
    p[10] = AdcStream_IsRunning();
    p[11] = 0;
    put_u32(&p[12], AdcStream_GetOverrunCount());
    put_u32(&p[16], AdcStream_GetSampleSeq());
    put_u32(&p[20], AdcStream_GetFlags());
    put_u16(&p[24], AdcStream_GetReadyCount());
    put_u16(&p[26], AdcStream_GetQueuedCount());
    put_u16(&p[28], AdcStream_GetFreeCount());
    put_u16(&p[30], (s_client != 0) ? tcp_sndbuf(s_client) : 0U);
    put_u32(&p[32], s_unacked_total_bytes);
    put_u32(&p[36], s_tcp_err_mem);
    put_u32(&p[40], s_tcp_acked_data_frames);

    err_t e = send_small_frame(APP_TYPE_ACK, p, sizeof(p));

    if (e == ERR_OK)
    {
        TCP_LOG_OK("TX ACK: status=%ld, fs=%lu, bits=%u, range=%u, running=%u, flags=0x%08lX, ready=%u, queued=%u, free=%u",
                   (long)status,
                   (unsigned long)g_app_cfg.fs_hz,
                   (unsigned int)g_app_cfg.bits,
                   (unsigned int)g_app_cfg.range,
                   (unsigned int)AdcStream_IsRunning(),
                   (unsigned long)AdcStream_GetFlags(),
                   (unsigned int)AdcStream_GetReadyCount(),
                   (unsigned int)AdcStream_GetQueuedCount(),
                   (unsigned int)AdcStream_GetFreeCount());
    }
    else
    {
        TCP_LOG_ERR("TX ACK failed: status=%ld, err=%d, sndbuf=%u, txq=%u",
                    (long)status,
                    (int)e,
                    (unsigned int)((s_client != 0) ? tcp_sndbuf(s_client) : 0U),
                    (unsigned int)s_txq_count);
    }

    return e;
}


static err_t send_net_ack(int32_t status)
{
    uint8_t p[36];

    put_u32(&p[0], (uint32_t)status);
    p[4] = g_app_net_cfg.server_ip[0];
    p[5] = g_app_net_cfg.server_ip[1];
    p[6] = g_app_net_cfg.server_ip[2];
    p[7] = g_app_net_cfg.server_ip[3];
    put_u16(&p[8], g_app_net_cfg.server_port);
    p[10] = g_app_net_cfg.device_id;
    p[11] = (s_client != 0) ? 1U : 0U;
    p[12] = s_reconnect_after_ack;
    p[13] = 0U;
    p[14] = 0U;
    p[15] = 0U;

    p[16] = g_app_net_cfg.local_ip[0];
    p[17] = g_app_net_cfg.local_ip[1];
    p[18] = g_app_net_cfg.local_ip[2];
    p[19] = g_app_net_cfg.local_ip[3];
    p[20] = g_app_net_cfg.netmask[0];
    p[21] = g_app_net_cfg.netmask[1];
    p[22] = g_app_net_cfg.netmask[2];
    p[23] = g_app_net_cfg.netmask[3];
    p[24] = g_app_net_cfg.gateway[0];
    p[25] = g_app_net_cfg.gateway[1];
    p[26] = g_app_net_cfg.gateway[2];
    p[27] = g_app_net_cfg.gateway[3];
    put_u32(&p[28], g_app_net_cfg.reconnect_ms);
    put_u32(&p[32], 0U);

    err_t e = send_small_frame(APP_TYPE_NET_ACK, p, sizeof(p));

    if (e == ERR_OK)
    {
        TCP_LOG_OK("TX NET_ACK: status=%ld, server=%u.%u.%u.%u:%u, dev=%u, reconnect_pending=%u",
                   (long)status,
                   (unsigned int)g_app_net_cfg.server_ip[0],
                   (unsigned int)g_app_net_cfg.server_ip[1],
                   (unsigned int)g_app_net_cfg.server_ip[2],
                   (unsigned int)g_app_net_cfg.server_ip[3],
                   (unsigned int)g_app_net_cfg.server_port,
                   (unsigned int)g_app_net_cfg.device_id,
                   (unsigned int)s_reconnect_after_ack);
    }
    else
    {
        TCP_LOG_ERR("TX NET_ACK failed: status=%ld, err=%d", (long)status, (int)e);
    }

    return e;
}

static err_t send_register_frame(void)
{
    uint8_t p[32];

    p[0] = g_app_net_cfg.device_id;
    p[1] = 0U;
    p[2] = 0U;
    p[3] = 0U;
    put_u32(&p[4], g_app_cfg.fs_hz);
    p[8] = g_app_cfg.bits;
    p[9] = g_app_cfg.range;
    p[10] = AdcStream_IsRunning();
    p[11] = 0U;
    put_u32(&p[12], AdcStream_GetSampleSeq());
    put_u32(&p[16], APP_PARAM_VERSION);
    put_u32(&p[20], 0U);
    put_u32(&p[24], 0U);
    put_u32(&p[28], 0U);

    return send_small_frame(APP_TYPE_REGISTER, p, sizeof(p));
}

static void drop_oldest_ready_for_backpressure(const char *reason)
{
    (void)reason;

    if (AdcStream_DropOldestReadyBlock() != 0U)
    {
        s_tcp_drop_ready_count++;
    }
}


static void stream_debug_reset_on_start(void)
{
    s_stream_start_tick = HAL_GetTick();
    s_last_stream_stat_tick = s_stream_start_tick;
    s_last_stream_seq = AdcStream_GetSampleSeq();
    s_last_stream_irq = AdcStream_GetDrdyIrqCount();
    s_no_drdy_warned = 0;
    s_no_data_warned = 0;
    s_stream_debug_active = 1;
    s_stream_stop_reported = 0;
    s_last_isr_stop_count = AdcStream_GetIsrStopCount();
    s_data_frame_too_large_warned = 0;
    s_tx_block_wait_count = 0;
    s_last_tx_frames = s_tcp_sent_frames;
    s_last_acked_frames = s_tcp_acked_data_frames;
    s_last_tcp_bytes = s_tcp_data_bytes_written;
    s_last_memerr = s_tcp_err_mem;
    s_last_overrun = AdcStream_GetOverrunCount();
    s_last_drop_ready = s_tcp_drop_ready_count;
}

static void stream_debug_print_periodic(void)
{
    uint32_t now;
    uint32_t seq;
    uint32_t flags;
    uint32_t overrun;
    uint8_t running;

    if (s_stream_debug_active == 0U)
    {
        return;
    }

    now = HAL_GetTick();
    running = AdcStream_IsRunning();
    seq = AdcStream_GetSampleSeq();
    flags = AdcStream_GetFlags();
    overrun = AdcStream_GetOverrunCount();

    if ((running == 0U) && (s_stream_stop_reported == 0U) &&
        (((now - s_stream_start_tick) > 50U) || (AdcStream_GetIsrStopCount() != s_last_isr_stop_count)))
    {
        s_stream_stop_reported = 1U;
        TCP_LOG_ERR("STREAM stopped: seq=%lu, status_new=%lu, ready=%u, queued=%u, free=%u, overrun=%lu, pend_drop=%lu, spi_err=%lu, flags=0x%08lX",
                    (unsigned long)seq,
                    (unsigned long)AdcStream_GetStatusNewCount(),
                    (unsigned int)AdcStream_GetReadyCount(),
                    (unsigned int)AdcStream_GetQueuedCount(),
                    (unsigned int)AdcStream_GetFreeCount(),
                    (unsigned long)overrun,
                    (unsigned long)ADS127L11_GetDmaPendingDropCount(),
                    (unsigned long)AdcStream_GetSpiErrorCount(),
                    (unsigned long)flags);
    }

    /* 连续 DMA 方案只用第一个 DRDY 做同步；启动后关闭 EXTI，后续 drdy_irq 不再增长是正常的。 */
    if (((now - s_stream_start_tick) > 1000U) &&
        (seq == 0U) &&
        (s_no_data_warned == 0U))
    {
        s_no_data_warned = 1U;
        TCP_LOG_ERR("STREAM no sample data after START: running=%u, status_new=%lu, status_skip=%lu, dma_chunk=%lu, dma_irq=%lu, pend_drop=%lu, spi_err=%lu, flags=0x%08lX, drdy_pin=%u",
                    (unsigned int)running,
                    (unsigned long)AdcStream_GetStatusNewCount(),
                    (unsigned long)AdcStream_GetStatusSkipCount(),
                    (unsigned long)AdcStream_GetContDmaChunkCount(),
                    (unsigned long)ADS127L11_GetDmaCompleteCount(),
                    (unsigned long)ADS127L11_GetDmaPendingDropCount(),
                    (unsigned long)AdcStream_GetSpiErrorCount(),
                    (unsigned long)flags,
                    (unsigned int)AdcStream_GetDrdyPinLevel());
    }

    if ((now - s_last_stream_stat_tick) < 1000U)
    {
        return;
    }

    uint32_t dt_ms = now - s_last_stream_stat_tick;
    if (dt_ms == 0U)
    {
        dt_ms = 1U;
    }
    s_last_stream_stat_tick = now;

#if (ADC_STREAM_STAT_LOG_ENABLE != 0U)
    uint32_t seq_delta = seq - s_last_stream_seq;
    uint32_t tx_delta = s_tcp_sent_frames - s_last_tx_frames;
    uint32_t ack_delta = s_tcp_acked_data_frames - s_last_acked_frames;
    uint32_t byte_delta = s_tcp_data_bytes_written - s_last_tcp_bytes;
    uint32_t mem_delta = s_tcp_err_mem - s_last_memerr;
    uint32_t ov_delta = overrun - s_last_overrun;
    uint32_t drop_delta = s_tcp_drop_ready_count - s_last_drop_ready;

    uint32_t seq_per_s = (uint32_t)(((uint64_t)seq_delta * 1000ULL) / (uint64_t)dt_ms);
    uint32_t tx_per_s = (uint32_t)(((uint64_t)tx_delta * 1000ULL) / (uint64_t)dt_ms);
    uint32_t ack_per_s = (uint32_t)(((uint64_t)ack_delta * 1000ULL) / (uint64_t)dt_ms);
    uint32_t kb_per_s = (uint32_t)(((uint64_t)byte_delta * 1000ULL) / ((uint64_t)dt_ms * 1024ULL));
    uint32_t drop_per_s = (uint32_t)(((uint64_t)drop_delta * 1000ULL) / (uint64_t)dt_ms);

    TCP_LOG_INFO("STAT fs=%lu bits=%u run=%u adc=%lu/s seq=%lu tx=%lu/s ack=%lu/s net=%luKB/s ready=%u queued=%u free=%u ov=%lu(+%lu) drop=%lu(+%lu/s) mem=%lu(+%lu) wait=%lu no_buf=%lu no_seg=%lu wr_err=%lu sndbuf=%u sndq=%u/%u pend=%lu flags=0x%08lX",
                 (unsigned long)g_app_cfg.fs_hz,
                 (unsigned int)g_app_cfg.bits,
                 (unsigned int)running,
                 (unsigned long)seq_per_s,
                 (unsigned long)seq,
                 (unsigned long)tx_per_s,
                 (unsigned long)ack_per_s,
                 (unsigned long)kb_per_s,
                 (unsigned int)AdcStream_GetReadyCount(),
                 (unsigned int)AdcStream_GetQueuedCount(),
                 (unsigned int)AdcStream_GetFreeCount(),
                 (unsigned long)overrun,
                 (unsigned long)ov_delta,
                 (unsigned long)s_tcp_drop_ready_count,
                 (unsigned long)drop_per_s,
                 (unsigned long)s_tcp_err_mem,
                 (unsigned long)mem_delta,
                 (unsigned long)s_tx_block_wait_count,
                 (unsigned long)s_tcp_no_sndbuf_count,
                 (unsigned long)s_tcp_no_seg_count,
                 (unsigned long)s_tcp_write_err_count,
                 (unsigned int)((s_client != 0) ? tcp_sndbuf(s_client) : 0U),
                 (unsigned int)tcp_sndq_len_now(),
                 (unsigned int)TCP_SND_QUEUELEN,
                 (unsigned long)ADS127L11_GetDmaPendingDropCount(),
                 (unsigned long)flags);

    s_last_tx_frames = s_tcp_sent_frames;
    s_last_acked_frames = s_tcp_acked_data_frames;
    s_last_tcp_bytes = s_tcp_data_bytes_written;
    s_last_memerr = s_tcp_err_mem;
    s_last_overrun = overrun;
    s_last_drop_ready = s_tcp_drop_ready_count;
#endif

    if ((seq == s_last_stream_seq) && (AdcStream_IsRunning() != 0U))
    {
        TCP_LOG_ERR("STREAM seq not increasing: status_new=%lu status_skip=%lu dma_chunk=%lu dma_irq=%lu pend=%lu spi_err=%lu flags=0x%08lX",
                    (unsigned long)AdcStream_GetStatusNewCount(),
                    (unsigned long)AdcStream_GetStatusSkipCount(),
                    (unsigned long)AdcStream_GetContDmaChunkCount(),
                    (unsigned long)ADS127L11_GetDmaCompleteCount(),
                    (unsigned long)ADS127L11_GetDmaPendingDropCount(),
                    (unsigned long)AdcStream_GetSpiErrorCount(),
                    (unsigned long)flags);
    }

    s_last_stream_seq = seq;
    s_last_stream_irq = AdcStream_GetDrdyIrqCount();

    if ((running == 0U) && ((now - s_stream_start_tick) > 3000U))
    {
        s_stream_debug_active = 0U;
    }
}

static int discard_stale_stream_before_start(void)
{
    uint16_t ready = AdcStream_GetReadyCount();
    uint16_t queued = AdcStream_GetQueuedCount();
    uint32_t overrun = AdcStream_GetOverrunCount();
    uint32_t flags = AdcStream_GetFlags();

    if ((ready == 0U) && (queued == 0U) && (overrun == 0U) && (flags == 0U))
    {
        return 0;
    }

    if (s_unacked_data_bytes != 0U)
    {
        TCP_LOG_ERR("ACTION START rejected: previous TCP data not ACKed, ready=%u queued=%u unacked_data=%lu overrun=%lu flags=0x%08lX",
                    (unsigned int)ready,
                    (unsigned int)queued,
                    (unsigned long)s_unacked_data_bytes,
                    (unsigned long)overrun,
                    (unsigned long)flags);
        return -1;
    }

    TCP_LOG_INFO("ACTION START: discard stale stream data before restart, ready=%u queued=%u overrun=%lu flags=0x%08lX",
                 (unsigned int)ready,
                 (unsigned int)queued,
                 (unsigned long)overrun,
                 (unsigned long)flags);

    AdcStream_ResetDiscard();
    return 0;
}

static int start_stream_now(void)
{
    TCP_LOG_INFO("ACTION START: AdcStream_Start fs=%lu bits=%u range=%u",
                 (unsigned long)g_app_cfg.fs_hz,
                 (unsigned int)g_app_cfg.bits,
                 (unsigned int)g_app_cfg.range);

    if (AdcStream_Start(&g_app_cfg) != 0)
    {
        TCP_LOG_ERR("ACTION START failed: AdcStream_Start returned error");
        return -1;
    }

    stream_debug_reset_on_start();

    TCP_LOG_OK("ACTION START done: stream running=%u, drdy_pin=%u",
               (unsigned int)AdcStream_IsRunning(),
               (unsigned int)AdcStream_GetDrdyPinLevel());
    s_resume_stream_after_reconnect = 0U;
    return 0;
}

static void handle_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (type == APP_TYPE_SET_PARAM)
    {
        TCP_LOG_INFO("CMD SET_PARAM received: payload_len=%u", (unsigned int)len);

        if (len < 8U)
        {
            TCP_LOG_ERR("CMD SET_PARAM invalid length: %u", (unsigned int)len);
            (void)send_ack(-10);
            return;
        }

        AppConfig_t cfg;
        cfg.fs_hz = get_u32(&payload[0]);
        cfg.bits = payload[4];
        cfg.range = payload[5];
        cfg.reserved0 = 0;
        cfg.reserved1 = 0;

        TCP_LOG_INFO("CMD SET_PARAM params: fs=%lu bits=%u range=%u",
                     (unsigned long)cfg.fs_hz,
                     (unsigned int)cfg.bits,
                     (unsigned int)cfg.range);

        if (!AppConfig_IsValid(&cfg))
        {
            TCP_LOG_ERR("CMD SET_PARAM rejected: invalid params");
            (void)send_ack(-11);
            return;
        }

        s_start_wait_ack = 0;
        s_start_do_now = 0;
        s_stream_requested = 0;
        s_resume_stream_after_reconnect = 0;

        TCP_LOG_INFO("ACTION SET_PARAM: stop stream before reconfigure");
        AdcStream_Stop();

        g_app_cfg = cfg;

        if (RangeCtrl_Set(g_app_cfg.range) != 0)
        {
            TCP_LOG_ERR("ACTION SET_PARAM failed: RangeCtrl_Set range=%u", (unsigned int)g_app_cfg.range);
            (void)send_ack(-14);
            return;
        }
        TCP_LOG_OK("ACTION SET_PARAM: RangeCtrl_Set done");

        if (ADS127L11_Configure(&g_app_cfg) != 0)
        {
            TCP_LOG_ERR("ACTION SET_PARAM failed: ADS127L11_Configure");
            (void)send_ack(-12);
            return;
        }
        TCP_LOG_OK("ACTION SET_PARAM: ADS127L11_Configure done");

        if (FlashParam_Save(&g_app_cfg) != 0)
        {
            TCP_LOG_ERR("ACTION SET_PARAM failed: FlashParam_Save");
            (void)send_ack(-13);
            return;
        }
        TCP_LOG_OK("ACTION SET_PARAM: FlashParam_Save done");

        (void)send_ack(0);
        return;
    }

    if (type == APP_TYPE_GET_PARAM)
    {
        (void)payload;
        (void)len;

        TCP_LOG_INFO("CMD GET_PARAM received");
        TCP_LOG_INFO("ACTION GET_PARAM: fs=%lu bits=%u range=%u running=%u",
                     (unsigned long)g_app_cfg.fs_hz,
                     (unsigned int)g_app_cfg.bits,
                     (unsigned int)g_app_cfg.range,
                     (unsigned int)AdcStream_IsRunning());

        (void)send_ack(0);
        return;
    }

    if (type == APP_TYPE_START)
    {
        (void)payload;
        (void)len;

        TCP_LOG_INFO("CMD START received: fs=%lu bits=%u range=%u",
                     (unsigned long)g_app_cfg.fs_hz,
                     (unsigned int)g_app_cfg.bits,
                     (unsigned int)g_app_cfg.range);

        if (AdcStream_IsRunning())
        {
            TCP_LOG_INFO("ACTION START ignored: stream already running");
            s_stream_requested = 1U;
            s_resume_stream_after_reconnect = 0U;
            (void)send_ack(0);
            return;
        }

        /*
         * START 前如果上一次采集发生 overrun，会留下 READY 数据块和 flags。
         * 如果这些数据没有处于 TCP 零拷贝未确认状态，就直接丢弃旧数据，
         * 让新的 START 可以重新开始，而不是一直返回 -22。
         */
        if (discard_stale_stream_before_start() != 0)
        {
            (void)send_ack(-22); /* 还有 TCP 未确认的旧数据，稍后再 START 或断开重连 */
            return;
        }

        if (ADS127L11_Configure(&g_app_cfg) != 0)
        {
            TCP_LOG_ERR("ACTION START failed: ADS127L11_Configure");
            (void)send_ack(-20);
            return;
        }
        TCP_LOG_OK("ACTION START: ADS127L11_Configure done");

        if (RangeCtrl_Set(g_app_cfg.range) != 0)
        {
            TCP_LOG_ERR("ACTION START failed: RangeCtrl_Set range=%u", (unsigned int)g_app_cfg.range);
            (void)send_ack(-23);
            return;
        }
        TCP_LOG_OK("ACTION START: RangeCtrl_Set done");

        if (send_ack(0) == ERR_OK)
        {
            /* 等 START ACK 被 TCP 确认后，再启动采集。 */
            s_start_wait_ack = 1;
            s_start_do_now = 0;
            s_stream_requested = 1U;
            s_resume_stream_after_reconnect = 0U;
            TCP_LOG_INFO("ACTION START: ACK sent, wait TCP sent callback before real sampling");
        }
        return;
    }

    if (type == APP_TYPE_STOP)
    {
        (void)payload;
        (void)len;

        TCP_LOG_INFO("CMD STOP received");

        s_start_wait_ack = 0;
        s_start_do_now = 0;
        s_stream_requested = 0;
        s_resume_stream_after_reconnect = 0;

        TCP_LOG_INFO("ACTION STOP: AdcStream_Stop");
        AdcStream_Stop();
        TCP_LOG_OK("ACTION STOP done: running=%u, ready=%u queued=%u flags=0x%08lX",
                   (unsigned int)AdcStream_IsRunning(),
                   (unsigned int)AdcStream_GetReadyCount(),
                   (unsigned int)AdcStream_GetQueuedCount(),
                   (unsigned long)AdcStream_GetFlags());

        /* STOP 后可能会把半包数据 flush 成 READY。ACK 必须排在所有数据之后。 */
        s_pending_stop_ack = 1;
        TCP_LOG_INFO("ACTION STOP: pending ACK after queued data drained");
        AdcTcpServer_Task();
        return;
    }


    if (type == APP_TYPE_GET_NET)
    {
        (void)payload;
        (void)len;

        TCP_LOG_INFO("CMD GET_NET received");
        (void)send_net_ack(0);
        return;
    }

    if (type == APP_TYPE_SET_NET)
    {
        AppNetConfig_t new_net;
        uint8_t flags;
        int32_t status = 0;

        TCP_LOG_INFO("CMD SET_NET received: payload_len=%u", (unsigned int)len);

        if (len < 8U)
        {
            TCP_LOG_ERR("CMD SET_NET invalid length: %u", (unsigned int)len);
            (void)send_net_ack(-30);
            return;
        }

        if (AdcStream_IsRunning())
        {
            TCP_LOG_ERR("CMD SET_NET rejected: stream is running, STOP first");
            (void)send_net_ack(-33);
            return;
        }

        new_net = g_app_net_cfg;
        new_net.server_ip[0] = payload[0];
        new_net.server_ip[1] = payload[1];
        new_net.server_ip[2] = payload[2];
        new_net.server_ip[3] = payload[3];
        new_net.server_port = get_u16(&payload[4]);
        new_net.device_id = payload[6];
        new_net.reserved0 = 0U;
        flags = payload[7];

        /*
         * 兼容两种 SET_NET payload：
         *   8字节：只改 server_ip/server_port/device_id/flags，适合远程改公网服务器。
         *   28字节及以上：附带 local_ip/netmask/gateway/reconnect_ms，适合本地调试一次性改 LAN 参数。
         */
        if (len >= 28U)
        {
            new_net.local_ip[0] = payload[8];
            new_net.local_ip[1] = payload[9];
            new_net.local_ip[2] = payload[10];
            new_net.local_ip[3] = payload[11];
            new_net.netmask[0] = payload[12];
            new_net.netmask[1] = payload[13];
            new_net.netmask[2] = payload[14];
            new_net.netmask[3] = payload[15];
            new_net.gateway[0] = payload[16];
            new_net.gateway[1] = payload[17];
            new_net.gateway[2] = payload[18];
            new_net.gateway[3] = payload[19];
            new_net.reconnect_ms = get_u32(&payload[20]);
        }

        TCP_LOG_INFO("CMD SET_NET params: server=%u.%u.%u.%u:%u, dev=%u, local=%u.%u.%u.%u, gw=%u.%u.%u.%u, reconnect=%lums, flags=0x%02X",
                     (unsigned int)new_net.server_ip[0],
                     (unsigned int)new_net.server_ip[1],
                     (unsigned int)new_net.server_ip[2],
                     (unsigned int)new_net.server_ip[3],
                     (unsigned int)new_net.server_port,
                     (unsigned int)new_net.device_id,
                     (unsigned int)new_net.local_ip[0],
                     (unsigned int)new_net.local_ip[1],
                     (unsigned int)new_net.local_ip[2],
                     (unsigned int)new_net.local_ip[3],
                     (unsigned int)new_net.gateway[0],
                     (unsigned int)new_net.gateway[1],
                     (unsigned int)new_net.gateway[2],
                     (unsigned int)new_net.gateway[3],
                     (unsigned long)new_net.reconnect_ms,
                     (unsigned int)flags);

        if (!AppNetConfig_IsValid(&new_net))
        {
            TCP_LOG_ERR("CMD SET_NET rejected: invalid net params");
            (void)send_net_ack(-31);
            return;
        }

        g_app_net_cfg = new_net;

        if ((flags & APP_NET_SET_FLAG_SAVE) != 0U)
        {
            if (FlashParam_SaveNet(&g_app_net_cfg) != 0)
            {
                TCP_LOG_ERR("CMD SET_NET failed: FlashParam_SaveNet");
                status = -32;
            }
            else
            {
                TCP_LOG_OK("CMD SET_NET: saved to Flash");
            }
        }

        if (send_net_ack(status) == ERR_OK)
        {
            if ((status == 0) && ((flags & APP_NET_SET_FLAG_RECONNECT) != 0U))
            {
                s_reconnect_after_ack = 1U;
                TCP_LOG_INFO("CMD SET_NET: reconnect after NET_ACK is TCP-ACKed");
            }
        }
        return;
    }

    TCP_LOG_ERR("CMD UNKNOWN received: type=0x%02X len=%u", (unsigned int)type, (unsigned int)len);
    (void)send_ack(-99);
}

static void parse_rx_stream(const uint8_t *data, uint16_t len)
{
    if (len > sizeof(s_rxbuf) - s_rxlen)
    {
        TCP_LOG_ERR("RX buffer overflow: incoming=%u buffered=%u", (unsigned int)len, (unsigned int)s_rxlen);
        s_rxlen = 0;
        return;
    }

    memcpy(&s_rxbuf[s_rxlen], data, len);
    s_rxlen = (uint16_t)(s_rxlen + len);

    while (s_rxlen >= APP_FRAME_HDR_LEN + APP_FRAME_CRC_LEN)
    {
        if (get_u16(&s_rxbuf[0]) != APP_PROTO_MAGIC)
        {
            memmove(s_rxbuf, &s_rxbuf[1], --s_rxlen);
            continue;
        }

        uint16_t payload_len = get_u16(&s_rxbuf[4]);
        if (payload_len > sizeof(s_rxbuf) - APP_FRAME_HDR_LEN - APP_FRAME_CRC_LEN)
        {
            TCP_LOG_ERR("RX invalid payload length: %u", (unsigned int)payload_len);
            s_rxlen = 0;
            return;
        }

        uint16_t total = (uint16_t)(APP_FRAME_HDR_LEN + payload_len + APP_FRAME_CRC_LEN);
        if (s_rxlen < total)
        {
            return;
        }

        uint8_t frame_ver = s_rxbuf[2];
        uint8_t frame_type = s_rxbuf[3];
        uint32_t frame_seq = get_u32(&s_rxbuf[6]);
        uint16_t rx_crc = get_u16(&s_rxbuf[APP_FRAME_HDR_LEN + payload_len]);
        uint16_t calc = crc16_modbus(s_rxbuf, APP_FRAME_HDR_LEN + payload_len);

        if ((frame_ver == APP_PROTO_VER) && (rx_crc == calc))
        {
            TCP_LOG_INFO("RX CMD frame: type=0x%02X(%s), seq=%lu, payload_len=%u",
                         (unsigned int)frame_type,
                         cmd_name(frame_type),
                         (unsigned long)frame_seq,
                         (unsigned int)payload_len);

            handle_frame(frame_type, &s_rxbuf[APP_FRAME_HDR_LEN], payload_len);
        }
        else
        {
            TCP_LOG_ERR("RX frame rejected: type=0x%02X(%s), seq=%lu, ver=%u, rx_crc=0x%04X, calc=0x%04X",
                        (unsigned int)frame_type,
                        cmd_name(frame_type),
                        (unsigned long)frame_seq,
                        (unsigned int)frame_ver,
                        (unsigned int)rx_crc,
                        (unsigned int)calc);
        }

        s_rxlen = (uint16_t)(s_rxlen - total);
        if (s_rxlen != 0U)
        {
            memmove(s_rxbuf, &s_rxbuf[total], s_rxlen);
        }
    }
}

static void adc_tcp_runtime_reset_on_connected(void)
{
    s_rxlen = 0;
    txq_reset();

    if (!AdcStream_IsRunning())
    {
        AdcStream_ResetDiscard();
    }

    s_pending_stop_ack = 0;
    s_start_wait_ack = 0;
    s_start_do_now = 0;
    s_reconnect_after_ack = 0;

    s_tcp_err_mem = 0;
    s_tcp_sndq_full = 0;
    s_tcp_sent_frames = 0;
    s_tcp_acked_data_frames = 0;
    s_tcp_data_bytes_written = 0;
    s_tcp_write_err_count = 0;
    s_tcp_no_sndbuf_count = 0;
    s_tcp_no_seg_count = 0;
    s_tcp_drop_ready_count = 0;
}

static void adc_tcp_schedule_reconnect(uint32_t delay_ms)
{
    s_connecting = 0U;
    s_connect_pcb = 0;
    s_client = 0;
    s_next_connect_tick = HAL_GetTick() + delay_ms;
}

static void adc_tcp_close_current(uint8_t reconnect)
{
    struct tcp_pcb *pcb = s_client;

    if ((reconnect != 0U) && (s_stream_requested != 0U))
    {
        s_resume_stream_after_reconnect = 1U;
    }

    AdcStream_Stop();

    s_client = 0;
    s_connect_pcb = 0;
    s_connecting = 0U;
    s_rxlen = 0;
    s_start_wait_ack = 0;
    s_start_do_now = 0;
    s_stream_debug_active = 0;
    txq_reset();

    if (pcb != 0)
    {
        tcp_arg(pcb, 0);
        tcp_recv(pcb, 0);
        tcp_sent(pcb, 0);
        tcp_poll(pcb, 0, 0);
        tcp_err(pcb, 0);

        if (tcp_close(pcb) != ERR_OK)
        {
            tcp_abort(pcb);
        }
    }

    if (reconnect != 0U)
    {
        s_next_connect_tick = HAL_GetTick() + 100U;
    }
}

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)arg;
    (void)tpcb;

    uint32_t n = len;

    if (s_unacked_total_bytes >= n)
    {
        s_unacked_total_bytes -= n;
    }
    else
    {
        s_unacked_total_bytes = 0;
    }

    while ((n > 0U) && (s_txq_count > 0U))
    {
        TxQueueItem_t *it = &s_txq[s_txq_head];

        if (n < it->len)
        {
            it->len = (uint16_t)(it->len - n);
            if (it->type == TXQ_TYPE_DATA)
            {
                if (s_unacked_data_bytes >= n) s_unacked_data_bytes -= n;
                else s_unacked_data_bytes = 0;
            }
            n = 0;
            break;
        }

        n -= it->len;
        if (it->type == TXQ_TYPE_DATA)
        {
            if (s_unacked_data_bytes >= it->len) s_unacked_data_bytes -= it->len;
            else s_unacked_data_bytes = 0;
            s_tcp_acked_data_frames++;
            if (it->blk != 0)
            {
                AdcStream_ReleaseQueuedBlock();
            }
        }

        memset(it, 0, sizeof(*it));
        s_txq_head = txq_next(s_txq_head);
        s_txq_count--;
    }

    if ((s_start_wait_ack != 0U) && (s_txq_count == 0U))
    {
        s_start_wait_ack = 0U;
        s_start_do_now = 1U;
        TCP_LOG_OK("START ACK confirmed by TCP, real sampling will start in task");
    }

    if ((s_reconnect_after_ack != 0U) && (s_txq_count == 0U))
    {
        TCP_LOG_OK("NET_ACK confirmed by TCP, reconnect will start in task");
    }

    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;

    if ((err != ERR_OK) || (p == 0))
    {
        TCP_LOG_INFO("TCP recv close/error: err=%d, p_present=%u", (int)err, (unsigned int)(p != 0));

        if (p != 0)
        {
            pbuf_free(p);
        }

        if (s_client == tpcb)
        {
            adc_tcp_close_current(1U);
        }

        TCP_LOG_OK("TCP disconnected by server, stream stopped, wait reconnect, resume=%u",
                   (unsigned int)s_resume_stream_after_reconnect);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    TCP_LOG_INFO("TCP RX bytes: tot_len=%u", (unsigned int)p->tot_len);

    struct pbuf *q = p;
    while (q != 0)
    {
        parse_rx_stream((const uint8_t *)q->payload, q->len);
        q = q->next;
    }

    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg;

    TCP_LOG_ERR("TCP error callback: err=%d", (int)err);

    if (s_stream_requested != 0U)
    {
        s_resume_stream_after_reconnect = 1U;
    }

    AdcStream_Stop();

    s_client = 0;
    s_connect_pcb = 0;
    s_connecting = 0U;
    s_rxlen = 0;
    s_start_wait_ack = 0;
    s_start_do_now = 0;
    s_reconnect_after_ack = 0;
    s_stream_debug_active = 0;
    txq_reset();

    s_next_connect_tick = HAL_GetTick() + g_app_net_cfg.reconnect_ms;

    TCP_LOG_OK("TCP error handled, stream stopped, wait reconnect, resume=%u",
               (unsigned int)s_resume_stream_after_reconnect);
}

static err_t on_poll(void *arg, struct tcp_pcb *tpcb)
{
    (void)arg;
    (void)tpcb;

    AdcTcpServer_Task();
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    err_t ack_e;

    (void)arg;

    if (err != ERR_OK)
    {
        TCP_LOG_ERR("TCP connect callback failed: err=%d", (int)err);
        s_connecting = 0U;
        s_connect_pcb = 0;
        s_client = 0;
        s_next_connect_tick = HAL_GetTick() + g_app_net_cfg.reconnect_ms;
        return err;
    }

    s_client = tpcb;
    s_connect_pcb = 0;
    s_connecting = 0U;

    tcp_recv(tpcb, on_recv);
    tcp_sent(tpcb, on_sent);
    tcp_err(tpcb, on_err);
    tcp_poll(tpcb, on_poll, 1);
    tcp_nagle_disable(tpcb);

#ifdef TCP_PRIO_MAX
    tcp_setprio(tpcb, TCP_PRIO_MAX);
#endif

    adc_tcp_runtime_reset_on_connected();

    TCP_LOG_OK("TCP connected to server: remote=%s:%u, local_port=%u, device_id=%u",
               ipaddr_ntoa(&tpcb->remote_ip),
               (unsigned int)tpcb->remote_port,
               (unsigned int)tpcb->local_port,
               (unsigned int)g_app_net_cfg.device_id);

    (void)send_register_frame();
    ack_e = send_ack(0);
    if ((ack_e == ERR_OK) && (s_resume_stream_after_reconnect != 0U) && (s_stream_requested != 0U))
    {
        s_start_wait_ack = 1U;
        s_start_do_now = 0U;
        TCP_LOG_INFO("AUTO RESUME: wait REGISTER/ACK TCP confirmation before sampling");
    }
    return ERR_OK;
}

static void adc_tcp_try_connect(void)
{
    ip_addr_t remote;
    uint32_t now = HAL_GetTick();
    err_t e;
    char ipbuf[24];

    if ((s_client != 0) || (s_connecting != 0U) || (s_connect_pcb != 0))
    {
        return;
    }

    if ((int32_t)(now - s_next_connect_tick) < 0)
    {
        return;
    }

    if (!AppNetConfig_IsValid(&g_app_net_cfg))
    {
        g_app_net_cfg = AppNetConfig_Default();
    }

    s_connect_pcb = tcp_new();
    if (s_connect_pcb == 0)
    {
        TCP_LOG_ERR("tcp_new failed, reconnect later");
        s_next_connect_tick = now + g_app_net_cfg.reconnect_ms;
        return;
    }

    IP4_ADDR(&remote,
             g_app_net_cfg.server_ip[0],
             g_app_net_cfg.server_ip[1],
             g_app_net_cfg.server_ip[2],
             g_app_net_cfg.server_ip[3]);

    tcp_arg(s_connect_pcb, 0);
    tcp_err(s_connect_pcb, on_err);
    tcp_poll(s_connect_pcb, on_poll, 1);
    tcp_nagle_disable(s_connect_pcb);

#ifdef TCP_PRIO_MAX
    tcp_setprio(s_connect_pcb, TCP_PRIO_MAX);
#endif

    AppNetConfig_FormatIp(&g_app_net_cfg, ipbuf, sizeof(ipbuf));
    s_connect_attempt_count++;
    TCP_LOG_INFO("TCP client connect attempt #%lu: server=%s:%u, dev=%u",
                 (unsigned long)s_connect_attempt_count,
                 ipbuf,
                 (unsigned int)g_app_net_cfg.server_port,
                 (unsigned int)g_app_net_cfg.device_id);

    e = tcp_connect(s_connect_pcb, &remote, g_app_net_cfg.server_port, on_connected);
    if (e != ERR_OK)
    {
        TCP_LOG_ERR("tcp_connect failed: err=%d", (int)e);
        tcp_arg(s_connect_pcb, 0);
        tcp_err(s_connect_pcb, 0);
        tcp_poll(s_connect_pcb, 0, 0);
        tcp_abort(s_connect_pcb);
        s_connect_pcb = 0;
        s_connecting = 0U;
        s_next_connect_tick = now + g_app_net_cfg.reconnect_ms;
        return;
    }

    s_connecting = 1U;
    s_next_connect_tick = now + g_app_net_cfg.reconnect_ms;
}

void AdcTcpServer_Init(void)
{
    char ipbuf[24];

    g_app_net_cfg = AppNetConfig_Default();
    if (FlashParam_LoadNet(&g_app_net_cfg) != 0)
    {
        g_app_net_cfg = AppNetConfig_Default();
    }

    AppNetConfig_FormatIp(&g_app_net_cfg, ipbuf, sizeof(ipbuf));

    TCP_LOG_INFO("TCP active client init: server=%s:%u, device_id=%u, reconnect=%lums",
                 ipbuf,
                 (unsigned int)g_app_net_cfg.server_port,
                 (unsigned int)g_app_net_cfg.device_id,
                 (unsigned long)g_app_net_cfg.reconnect_ms);

    s_client = 0;
    s_connect_pcb = 0;
    s_connecting = 0U;
    s_next_connect_tick = HAL_GetTick() + 500U;
    s_connect_attempt_count = 0U;
    txq_reset();
}

void AdcTcpServer_Task(void)
{
    uint8_t wrote_any = 0;
    uint16_t burst_count = 0;

    /* 处理 ISR 延迟停采请求，避免在 EXTI 中断里调用 SPI 停止动作。 */
    AdcStream_Task();

    if (s_client == 0)
    {
        adc_tcp_try_connect();
        return;
    }

    if ((s_reconnect_after_ack != 0U) && (s_txq_count == 0U))
    {
        TCP_LOG_INFO("Reconnect requested and TX queue empty, close current TCP and reconnect");
        adc_tcp_close_current(1U);
        return;
    }

    if (s_start_do_now != 0U)
    {
        s_start_do_now = 0;
        if (start_stream_now() != 0)
        {
            (void)send_ack(-21);
        }
    }

    stream_debug_print_periodic();

    AdcStreamBlock_t *blk = AdcStream_PeekReadyBlock();
    while (blk != 0)
    {
        uint16_t sndbuf_now;
        uint16_t sndq_now;
        uint16_t need_seg;
        uint16_t sndq_limit;
        err_t e;

        if (s_txq_count >= TXQ_LEN)
        {
            s_tcp_err_mem++;
            s_tx_block_wait_count++;
            drop_oldest_ready_for_backpressure("txq_full");
            break;
        }

        build_data_frame(blk);
        need_seg = tcp_required_segs(blk->frame_len);
        sndbuf_now = tcp_sndbuf(s_client);
        sndq_now = tcp_sndq_len_now();
        sndq_limit = (TCP_SND_QUEUELEN > 4U) ? (uint16_t)(TCP_SND_QUEUELEN - 4U) : (uint16_t)TCP_SND_QUEUELEN;

        /*
         * 新发送策略：
         * 1. 一个 DATA block 约 2*MSS，减少应用帧数量和 CRC/tcp_write 调度次数；
         * 2. 零拷贝 tcp_write，block 等 tcp_sent 完整确认后再释放；
         * 3. 一轮尽量写多个 block，最后统一 tcp_output；
         * 4. ERR_MEM/空间不足时不取走 READY block，下一轮继续发送，避免制造缺样。
         */
        if (sndbuf_now < blk->frame_len)
        {
            s_tcp_err_mem++;
            s_tcp_no_sndbuf_count++;
            s_tx_block_wait_count++;
            drop_oldest_ready_for_backpressure("sndbuf");
            break;
        }

        if ((uint16_t)(sndq_now + need_seg) >= sndq_limit)
        {
            s_tcp_err_mem++;
            s_tcp_no_seg_count++;
            s_tcp_sndq_full++;
            s_tx_block_wait_count++;
            drop_oldest_ready_for_backpressure("sndq");
            break;
        }

        if (txq_push(TXQ_TYPE_DATA, blk->frame_len, 0) != 0)
        {
            s_tcp_err_mem++;
            s_tx_block_wait_count++;
            drop_oldest_ready_for_backpressure("txq_push");
            break;
        }

        e = tcp_write(s_client, blk->frame, blk->frame_len, (uint8_t)(TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE));
        if (e != ERR_OK)
        {
            txq_pop_last();
            s_tcp_err_mem++;
            s_tcp_write_err_count++;
            s_tx_block_wait_count++;
            drop_oldest_ready_for_backpressure("tcp_write");
            break;
        }

        s_tcp_sent_frames++;
        s_tcp_data_bytes_written += blk->data_bytes;
        s_tx_block_wait_count = 0;
        wrote_any = 1U;

#if (ADC_TCP_DATA_TRACE_ENABLE != 0U)
        if ((s_tcp_sent_frames <= 8U) || ((s_tcp_sent_frames % 100U) == 0U))
        {
            TCP_LOG_INFO("TX DATA no=%lu first=%lu samples=%u bytes=%u frame=%u sndbuf=%u sndq=%u/%u txq=%u",
                         (unsigned long)s_tcp_sent_frames,
                         (unsigned long)blk->first_sample_seq,
                         (unsigned int)blk->sample_count,
                         (unsigned int)blk->data_bytes,
                         (unsigned int)blk->frame_len,
                         (unsigned int)tcp_sndbuf(s_client),
                         (unsigned int)tcp_sndq_len_now(),
                         (unsigned int)TCP_SND_QUEUELEN,
                         (unsigned int)s_txq_count);
        }
#endif

        AdcStream_ReleaseReadyBlock();

        burst_count++;
        if ((burst_count >= APP_TCP_SEND_BURST_MAX) || ((burst_count & 1U) == 0U))
        {
            /* 穿插处理 SPI DMA pending，避免只顾 TCP 导致 DMA pending 堆积。 */
            AdcStream_Task();
        }

        if (burst_count >= APP_TCP_SEND_BURST_MAX)
        {
            break;
        }

        blk = AdcStream_PeekReadyBlock();
    }

    if ((s_pending_stop_ack != 0) && (AdcStream_GetReadyCount() == 0U))
    {
        if (send_ack(s_pending_stop_ack > 0 ? 0 : s_pending_stop_ack) == ERR_OK)
        {
            s_pending_stop_ack = 0;
            wrote_any = 1;
            TCP_LOG_OK("STOP ACK sent after data drained");
        }
    }

    if (wrote_any != 0U)
    {
        (void)tcp_output(s_client);
    }
}
