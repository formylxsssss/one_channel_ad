#include "adc_tcp_server.h"
#include "app_config.h"
#include "flash_param.h"
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

static struct tcp_pcb *s_listen = 0;
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
static uint32_t s_tcp_sent_frames = 0;
static uint32_t s_tcp_acked_data_frames = 0;
static int32_t s_pending_stop_ack = 0;

/*
 * START 防卡死修改：
 * 1. 收到 START 后先配置 ADC 并发送 ACK；
 * 2. 等 START ACK 被 TCP 确认后，再在 AdcTcpServer_Task() 中真正启动采集。
 */
static uint8_t s_start_wait_ack = 0;
static uint8_t s_start_do_now = 0;

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

static uint16_t crc16_modbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
        }
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
}

static void stream_debug_print_periodic(void)
{
    uint32_t now;
    uint32_t seq;
    uint32_t irq;
    uint32_t flags;
    uint32_t spi_err;
    uint32_t overrun;
    uint32_t isr_stop_count;
    uint8_t running;

    if (s_stream_debug_active == 0U)
    {
        return;
    }

    now = HAL_GetTick();
    running = AdcStream_IsRunning();
    seq = AdcStream_GetSampleSeq();
    irq = AdcStream_GetDrdyIrqCount();
    flags = AdcStream_GetFlags();
    spi_err = AdcStream_GetSpiErrorCount();
    overrun = AdcStream_GetOverrunCount();
    isr_stop_count = AdcStream_GetIsrStopCount();

    if ((running == 0U) && (s_stream_stop_reported == 0U) &&
        (((now - s_stream_start_tick) > 50U) || (isr_stop_count != s_last_isr_stop_count)))
    {
        s_stream_stop_reported = 1U;
        TCP_LOG_ERR("STREAM stopped after START: seq=%lu, drdy_irq=%lu, partial=%u, ready=%u, queued=%u, free=%u, overrun=%lu, spi_err=%lu, dma_busy=%lu, dma_err=%lu, last_spi_ret=%ld, dma_init_err=%lu, hal_state=%lu, hal_err=0x%08lX, isr_stop=%lu, flags=0x%08lX, last_stop_flags=0x%08lX, drdy_pin=%u",
                    (unsigned long)seq,
                    (unsigned long)irq,
                    (unsigned int)AdcStream_GetPartialSamples(),
                    (unsigned int)AdcStream_GetReadyCount(),
                    (unsigned int)AdcStream_GetQueuedCount(),
                    (unsigned int)AdcStream_GetFreeCount(),
                    (unsigned long)overrun,
                    (unsigned long)spi_err,
                    (unsigned long)ADS127L11_GetDmaCompleteCount(),
                    (unsigned long)AdcStream_GetDmaBusyCount(),
                    (unsigned long)AdcStream_GetDmaErrorCount(),
                    (long)AdcStream_GetLastSpiRet(),
                    (unsigned long)ADS127L11_GetDmaInitError(),
                    (unsigned long)ADS127L11_GetLastHalSpiState(),
                    (unsigned long)ADS127L11_GetLastHalSpiError(),
                    (unsigned long)isr_stop_count,
                    (unsigned long)flags,
                    (unsigned long)AdcStream_GetLastStopFlags(),
                    (unsigned int)AdcStream_GetDrdyPinLevel());
    }

    /* START 后 1 秒内完全没有 DRDY 中断，基本就是 DRDY EXTI 没配好或 ADS 没出 DRDY。 */
    if (((now - s_stream_start_tick) > 1000U) &&
        (irq == 0U) &&
        (s_no_drdy_warned == 0U))
    {
        s_no_drdy_warned = 1U;
        TCP_LOG_ERR("STREAM no DRDY IRQ after START: running=%u, drdy_pin=%u. Check DRDY GPIO EXTI falling-edge, NVIC EXTI IRQ, ADS CLK/START/RESET",
                    (unsigned int)running,
                    (unsigned int)AdcStream_GetDrdyPinLevel());
    }

    /* START 后 1 秒内没有任何采样点进入缓存。 */
    if (((now - s_stream_start_tick) > 1000U) &&
        (seq == 0U) &&
        (s_no_data_warned == 0U))
    {
        s_no_data_warned = 1U;
        TCP_LOG_ERR("STREAM no sample data after START: running=%u, drdy_irq=%lu, dma_irq=%lu, spi_err=%lu, dma_busy=%lu, dma_err=%lu, last_spi_ret=%ld, dma_init_err=%lu, hal_state=%lu, hal_err=0x%08lX, flags=0x%08lX, drdy_pin=%u",
                    (unsigned int)running,
                    (unsigned long)irq,
                    (unsigned long)ADS127L11_GetDmaCompleteCount(),
                    (unsigned long)spi_err,
                    (unsigned long)ADS127L11_GetDmaCompleteCount(),
                    (unsigned long)AdcStream_GetDmaBusyCount(),
                    (unsigned long)AdcStream_GetDmaErrorCount(),
                    (long)AdcStream_GetLastSpiRet(),
                    (unsigned long)ADS127L11_GetDmaInitError(),
                    (unsigned long)ADS127L11_GetLastHalSpiState(),
                    (unsigned long)ADS127L11_GetLastHalSpiError(),
                    (unsigned long)flags,
                    (unsigned int)AdcStream_GetDrdyPinLevel());
    }

    if ((now - s_last_stream_stat_tick) < 1000U)
    {
        return;
    }

    s_last_stream_stat_tick = now;

    TCP_LOG_INFO("STREAM STAT: running=%u, seq=%lu(+%lu/s), drdy_irq=%lu(+%lu/s), dma_irq=%lu, partial=%u, ready=%u, queued=%u, free=%u, sndbuf=%u, unacked=%lu, tx_frames=%lu, acked_frames=%lu, overrun=%lu, spi_err=%lu, dma_busy=%lu, dma_err=%lu, last_spi_ret=%ld, dma_init_err=%lu, hal_state=%lu, hal_err=0x%08lX, isr_stop=%lu, flags=0x%08lX, drdy_pin=%u",
                 (unsigned int)running,
                 (unsigned long)seq,
                 (unsigned long)(seq - s_last_stream_seq),
                 (unsigned long)irq,
                 (unsigned long)(irq - s_last_stream_irq),
                 (unsigned long)ADS127L11_GetDmaCompleteCount(),
                 (unsigned int)AdcStream_GetPartialSamples(),
                 (unsigned int)AdcStream_GetReadyCount(),
                 (unsigned int)AdcStream_GetQueuedCount(),
                 (unsigned int)AdcStream_GetFreeCount(),
                 (unsigned int)((s_client != 0) ? tcp_sndbuf(s_client) : 0U),
                 (unsigned long)s_unacked_total_bytes,
                 (unsigned long)s_tcp_sent_frames,
                 (unsigned long)s_tcp_acked_data_frames,
                 (unsigned long)overrun,
                 (unsigned long)spi_err,
                 (unsigned long)AdcStream_GetDmaBusyCount(),
                 (unsigned long)AdcStream_GetDmaErrorCount(),
                 (long)AdcStream_GetLastSpiRet(),
                 (unsigned long)ADS127L11_GetDmaInitError(),
                 (unsigned long)ADS127L11_GetLastHalSpiState(),
                 (unsigned long)ADS127L11_GetLastHalSpiError(),
                 (unsigned long)isr_stop_count,
                 (unsigned long)flags,
                 (unsigned int)AdcStream_GetDrdyPinLevel());

    if ((seq == s_last_stream_seq) && (irq != s_last_stream_irq))
    {
        TCP_LOG_ERR("STREAM DMA/DRDY exists but sample seq not increasing: check ADS STATUS parsing/SPI frame/CS/SCK/MISO, dma_irq=%lu, dma_busy=%lu, dma_err=%lu, last_spi_ret=%ld, dma_init_err=%lu, hal_state=%lu, hal_err=0x%08lX, flags=0x%08lX",
                    (unsigned long)ADS127L11_GetDmaCompleteCount(),
                    (unsigned long)AdcStream_GetDmaBusyCount(),
                    (unsigned long)AdcStream_GetDmaErrorCount(),
                    (long)AdcStream_GetLastSpiRet(),
                    (unsigned long)ADS127L11_GetDmaInitError(),
                    (unsigned long)ADS127L11_GetLastHalSpiState(),
                    (unsigned long)ADS127L11_GetLastHalSpiError(),
                    (unsigned long)flags);
    }

    s_last_stream_seq = seq;
    s_last_stream_irq = irq;

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
            AdcStream_ReleaseQueuedBlock();
        }

        memset(it, 0, sizeof(*it));
        s_txq_head = txq_next(s_txq_head);
        s_txq_count--;
    }

    /* START ACK 已经被 TCP 确认，下一次 Task 再真正开采集。 */
    if ((s_start_wait_ack != 0U) && (s_txq_count == 0U))
    {
        s_start_wait_ack = 0;
        s_start_do_now = 1;
        TCP_LOG_OK("START ACK confirmed by TCP, real sampling will start in task");
    }

    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;

    if ((err != ERR_OK) || (p == 0))
    {
        TCP_LOG_INFO("TCP recv close/error: err=%d, p_present=%u", (int)err, (unsigned int)(p != 0));

        AdcStream_Stop();
        if (p != 0)
        {
            pbuf_free(p);
        }
        tcp_close(tpcb);
        if (s_client == tpcb)
        {
            s_client = 0;
        }
        s_rxlen = 0;
        s_start_wait_ack = 0;
        s_start_do_now = 0;
        s_stream_debug_active = 0;
        txq_reset();

        TCP_LOG_OK("TCP client disconnected, stream stopped");
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

    AdcStream_Stop();
    s_client = 0;
    s_rxlen = 0;
    s_start_wait_ack = 0;
    s_start_do_now = 0;
    s_stream_debug_active = 0;
    txq_reset();

    TCP_LOG_OK("TCP error handled, stream stopped");
}

static err_t on_poll(void *arg, struct tcp_pcb *tpcb)
{
    (void)arg;
    (void)tpcb;

    AdcTcpServer_Task();
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK)
    {
        TCP_LOG_ERR("TCP accept error: err=%d", (int)err);
        return err;
    }

    if (s_client != 0)
    {
        TCP_LOG_ERR("TCP reject new client: already connected, remote=%s:%u",
                    ipaddr_ntoa(&newpcb->remote_ip),
                    (unsigned int)newpcb->remote_port);
        tcp_close(newpcb);
        return ERR_ABRT;
    }

    s_client = newpcb;
    s_rxlen = 0;
    txq_reset();
    if (!AdcStream_IsRunning())
    {
        AdcStream_ResetDiscard();
    }
    s_pending_stop_ack = 0;
    s_start_wait_ack = 0;
    s_start_do_now = 0;
    s_tcp_err_mem = 0;
    s_tcp_sent_frames = 0;
    s_tcp_acked_data_frames = 0;

    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    tcp_err(newpcb, on_err);
    tcp_poll(newpcb, on_poll, 1);
    tcp_nagle_disable(newpcb);

#ifdef TCP_PRIO_MAX
    tcp_setprio(newpcb, TCP_PRIO_MAX);
#endif

    TCP_LOG_OK("TCP client connected: remote=%s:%u, local_port=%u",
               ipaddr_ntoa(&newpcb->remote_ip),
               (unsigned int)newpcb->remote_port,
               (unsigned int)newpcb->local_port);

    (void)send_ack(0);
    return ERR_OK;
}

void AdcTcpServer_Init(void)
{
    TCP_LOG_INFO("TCP server init start, port=%u", (unsigned int)APP_TCP_PORT);

    s_listen = tcp_new();
    if (s_listen == 0)
    {
        TCP_LOG_ERR("tcp_new failed");
        return;
    }

    err_t e = tcp_bind(s_listen, IP_ADDR_ANY, APP_TCP_PORT);
    if (e != ERR_OK)
    {
        TCP_LOG_ERR("tcp_bind failed: port=%u err=%d", (unsigned int)APP_TCP_PORT, (int)e);
        tcp_close(s_listen);
        s_listen = 0;
        return;
    }

    s_listen = tcp_listen(s_listen);
    if (s_listen == 0)
    {
        TCP_LOG_ERR("tcp_listen failed");
        return;
    }

    tcp_accept(s_listen, on_accept);
    TCP_LOG_OK("TCP server listening: port=%u", (unsigned int)APP_TCP_PORT);
}

void AdcTcpServer_Task(void)
{
    uint8_t wrote_any = 0;

    /* 处理 ISR 延迟停采请求，避免在 EXTI 中断里调用 SPI 停止动作。 */
    AdcStream_Task();

    if (s_client == 0)
    {
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
        if (s_txq_count >= TXQ_LEN)
        {
            s_tcp_err_mem++;
            break;
        }

        build_data_frame(blk);

        uint16_t sndbuf_now = tcp_sndbuf(s_client);
        if (sndbuf_now < blk->frame_len)
        {
            s_tcp_err_mem++;
            s_tx_block_wait_count++;

            /*
             * 这里是本次问题的关键保护：
             * 如果 DATA 帧长度长期大于 tcp_sndbuf，则 tcp_write 永远不会发送。
             * 旧版本 APP_ADC_BLOCK_SAMPLES=472 时，24bit frame_len=1444，
             * 而你现场 tcp_sndbuf=1072，所以会一直 ready 增加，最后 overrun。
             */
            if ((s_data_frame_too_large_warned == 0U) ||
                ((s_tx_block_wait_count % 1000UL) == 0UL))
            {
                s_data_frame_too_large_warned = 1U;
                TCP_LOG_ERR("TX DATA wait: frame_len=%u > sndbuf=%u, ready=%u, queued=%u, free=%u, wait_count=%lu. If this repeats, reduce APP_ADC_BLOCK_SAMPLES or increase LwIP TCP_SND_BUF/TCP_MSS",
                            (unsigned int)blk->frame_len,
                            (unsigned int)sndbuf_now,
                            (unsigned int)AdcStream_GetReadyCount(),
                            (unsigned int)AdcStream_GetQueuedCount(),
                            (unsigned int)AdcStream_GetFreeCount(),
                            (unsigned long)s_tx_block_wait_count);
            }

            if (wrote_any != 0U)
            {
                (void)tcp_output(s_client);
            }
            break;
        }

        /* 零拷贝：不使用 TCP_WRITE_FLAG_COPY。块必须等 tcp_sent 确认后才能释放。 */
        err_t e = tcp_write(s_client, blk->frame, blk->frame_len, 0);
        if (e != ERR_OK)
        {
            s_tcp_err_mem++;
            break;
        }

        if (txq_push(TXQ_TYPE_DATA, blk->frame_len, blk) != 0)
        {
            s_tcp_err_mem++;
            break;
        }

        s_tcp_sent_frames++;
        s_tx_block_wait_count = 0;


        #if (ADC_TCP_DATA_TRACE_ENABLE != 0U)
        if ((s_tcp_sent_frames <= 8U) || ((s_tcp_sent_frames % 100U) == 0U))
        {
            TCP_LOG_INFO("TX DATA frame: no=%lu, first_seq=%lu, samples=%u, data_bytes=%u, frame_len=%u, sndbuf=%u, txq=%u",
                         (unsigned long)s_tcp_sent_frames,
                         (unsigned long)blk->first_sample_seq,
                         (unsigned int)blk->sample_count,
                         (unsigned int)blk->data_bytes,
                         (unsigned int)blk->frame_len,
                         (unsigned int)tcp_sndbuf(s_client),
                         (unsigned int)s_txq_count);
        }
#endif

        wrote_any = 1;
        AdcStream_MarkBlockQueued(blk);
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
