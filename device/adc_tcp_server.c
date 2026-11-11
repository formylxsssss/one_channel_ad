#include "adc_tcp_server.h"
#include "adc_udp_protocol.h"
#include "app_net_config.h"
#include "flash_param.h"
#include "ads127l11.h"
#include "adc_stream.h"
#include "range_ctrl.h"
#include "main.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "SEGGER_RTT.h"
#include <string.h>

extern struct netif gnetif;
#define CTRL_QUEUE_LEN 8U
#define RX_QUEUE_LEN 4U

#if ADC_TCP_TRACE_ENABLE
#define UDP_LOG_INFO(...) do { SEGGER_RTT_printf(0, "[UDP] "); SEGGER_RTT_printf(0, __VA_ARGS__); SEGGER_RTT_printf(0, "\r\n"); } while (0)
#define UDP_LOG_OK UDP_LOG_INFO
#define UDP_LOG_ERR UDP_LOG_INFO
#else
#define UDP_LOG_INFO(...) do {} while (0)
#define UDP_LOG_OK(...) do {} while (0)
#define UDP_LOG_ERR(...) do {} while (0)
#endif

typedef struct { uint16_t len; uint8_t data[APP_ACK_FRAME_MAX_LEN]; } ControlFrame;
typedef struct { uint16_t len; uint8_t data[APP_UDP_COMMAND_MAX]; } CommandFrame;
static struct udp_pcb *s_udp;
static uint8_t s_connected, s_start_do_now, s_stream_requested;
static uint8_t s_resume_stream_after_reconnect, s_reconnect_after_ack;
static int32_t s_pending_stop_ack;
static uint32_t s_frame_seq, s_last_rx_tick, s_next_connect_tick, s_last_register_tick;
static uint32_t s_send_errors, s_sent_frames, s_sent_bytes, s_last_stat_tick, s_last_stat_seq;
static ControlFrame s_controls[CTRL_QUEUE_LEN];
static CommandFrame s_commands[RX_QUEUE_LEN];
static uint8_t s_ctrl_head, s_ctrl_count, s_rx_head, s_rx_count;
static uint8_t s_data[APP_UDP_MAX_PAYLOAD];
static uint16_t s_data_len, s_data_offset, s_data_count;

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

    uint16_t crc = AdcUdp_Crc16(out, APP_FRAME_HDR_LEN + len);
    put_u16(&out[APP_FRAME_HDR_LEN + len], crc);

    return (uint16_t)(APP_FRAME_HDR_LEN + len + APP_FRAME_CRC_LEN);
}


static err_t send_small_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    ControlFrame *out;
    if (!s_udp) return ERR_CONN;
    if ((uint32_t)len + 12U > APP_ACK_FRAME_MAX_LEN) return ERR_ARG;
    if (s_ctrl_count == CTRL_QUEUE_LEN) return ERR_MEM;
    out = &s_controls[(s_ctrl_head + s_ctrl_count) % CTRL_QUEUE_LEN];
    out->len = build_small_frame(type, s_frame_seq++, payload, len, out->data);
    s_ctrl_count++;
    return ERR_OK;
}

static err_t send_datagram(const uint8_t *data, uint16_t len)
{
    struct pbuf *p;
    err_t result;
    if (!s_udp) return ERR_CONN;
    p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) { s_send_errors++; return ERR_MEM; }
    result = pbuf_take(p, data, len);
    if (result == ERR_OK) result = udp_send(s_udp, p);
    pbuf_free(p);
    if (result != ERR_OK) s_send_errors++;
    return result;
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
    put_u16(&p[30], 0U);
    put_u32(&p[32], 0U);
    put_u32(&p[36], s_send_errors);
    put_u32(&p[40], 0U);

    err_t e = send_small_frame(APP_TYPE_ACK, p, sizeof(p));

    if (e == ERR_OK)
    {
        UDP_LOG_OK("TX ACK: status=%ld, fs=%lu, bits=%u, range=%u, running=%u, flags=0x%08lX, ready=%u, queued=%u, free=%u",
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
        UDP_LOG_ERR("TX ACK failed: status=%ld, err=%d, sndbuf=%u, txq=%u",
                    (long)status,
                    (int)e,
                    (unsigned int)(0U),
                    (unsigned int)s_ctrl_count);
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
    p[11] = s_connected;
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
        UDP_LOG_OK("TX NET_ACK: status=%ld, server=%u.%u.%u.%u:%u, dev=%u, reconnect_pending=%u",
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
        UDP_LOG_ERR("TX NET_ACK failed: status=%ld, err=%d", (long)status, (int)e);
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


static int discard_stale_stream_before_start(void)
{
    if (s_pending_stop_ack) return -1;
    AdcStream_ResetDiscard();
    s_data_len = s_data_offset = 0U;
    return 0;
}

static int start_stream_now(void)
{
    int result = AdcStream_Start(&g_app_cfg);
    if (result != 0) {
        s_stream_requested = s_resume_stream_after_reconnect = 0U;
        UDP_LOG_ERR("ADC start failed: %d", result);
        return result;
    }
    s_resume_stream_after_reconnect = 0U;
    s_last_stat_tick = HAL_GetTick();
    s_last_stat_seq = AdcStream_GetSampleSeq();
    UDP_LOG_INFO("Sampling started: selector=%lu actual=%lu bits=%u",
                 (unsigned long)g_app_cfg.fs_hz,
                 (unsigned long)AppConfig_ActualFsHz(&g_app_cfg), g_app_cfg.bits);
    return 0;
}

static void handle_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (type == APP_TYPE_SET_PARAM)
    {
        UDP_LOG_INFO("CMD SET_PARAM received: payload_len=%u", (unsigned int)len);

        if (len < 8U)
        {
            UDP_LOG_ERR("CMD SET_PARAM invalid length: %u", (unsigned int)len);
            (void)send_ack(-10);
            return;
        }

        AppConfig_t cfg;
        cfg.fs_hz = get_u32(&payload[0]);
        cfg.bits = payload[4];
        cfg.range = payload[5];
        cfg.reserved0 = 0;
        cfg.reserved1 = 0;

        UDP_LOG_INFO("CMD SET_PARAM params: fs=%lu bits=%u range=%u",
                     (unsigned long)cfg.fs_hz,
                     (unsigned int)cfg.bits,
                     (unsigned int)cfg.range);

        if (!AppConfig_IsValid(&cfg))
        {
            UDP_LOG_ERR("CMD SET_PARAM rejected: invalid params");
            (void)send_ack(-11);
            return;
        }
        s_start_do_now = 0;
        s_stream_requested = 0;
        s_resume_stream_after_reconnect = 0;

        UDP_LOG_INFO("ACTION SET_PARAM: stop stream before reconfigure");
        AdcStream_Stop();

        AdcStream_ResetDiscard();
        s_data_len = s_data_offset = 0U;
        g_app_cfg = cfg;

        if (RangeCtrl_Set(g_app_cfg.range) != 0)
        {
            UDP_LOG_ERR("ACTION SET_PARAM failed: RangeCtrl_Set range=%u", (unsigned int)g_app_cfg.range);
            (void)send_ack(-14);
            return;
        }
        UDP_LOG_OK("ACTION SET_PARAM: RangeCtrl_Set done");

        if (ADS127L11_Configure(&g_app_cfg) != 0)
        {
            UDP_LOG_ERR("ACTION SET_PARAM failed: ADS127L11_Configure");
            (void)send_ack(-12);
            return;
        }
        UDP_LOG_OK("ACTION SET_PARAM: ADS127L11_Configure done");

        if (FlashParam_Save(&g_app_cfg) != 0)
        {
            UDP_LOG_ERR("ACTION SET_PARAM failed: FlashParam_Save");
            (void)send_ack(-13);
            return;
        }
        UDP_LOG_OK("ACTION SET_PARAM: FlashParam_Save done");

        (void)send_ack(0);
        return;
    }

    if (type == APP_TYPE_GET_PARAM)
    {
        (void)payload;
        (void)len;

        UDP_LOG_INFO("CMD GET_PARAM received");
        UDP_LOG_INFO("ACTION GET_PARAM: fs=%lu bits=%u range=%u running=%u",
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

        UDP_LOG_INFO("CMD START received: fs=%lu bits=%u range=%u",
                     (unsigned long)g_app_cfg.fs_hz,
                     (unsigned int)g_app_cfg.bits,
                     (unsigned int)g_app_cfg.range);

        if (AdcStream_IsRunning())
        {
            UDP_LOG_INFO("ACTION START ignored: stream already running");
            s_stream_requested = 1U;
            s_resume_stream_after_reconnect = 0U;
            (void)send_ack(0);
            return;
        }

        /* Do not let a new START overtake an outstanding STOP response. */
        if (discard_stale_stream_before_start() != 0)
        {
            (void)send_ack(-22);
            return;
        }

        if (ADS127L11_Configure(&g_app_cfg) != 0)
        {
            UDP_LOG_ERR("ACTION START failed: ADS127L11_Configure");
            (void)send_ack(-20);
            return;
        }
        UDP_LOG_OK("ACTION START: ADS127L11_Configure done");

        if (RangeCtrl_Set(g_app_cfg.range) != 0)
        {
            UDP_LOG_ERR("ACTION START failed: RangeCtrl_Set range=%u", (unsigned int)g_app_cfg.range);
            (void)send_ack(-23);
            return;
        }
        UDP_LOG_OK("ACTION START: RangeCtrl_Set done");

        if (send_ack(0) == ERR_OK)
        {
            /* Start after the queued ACK has been handed to UDP, not on a nonexistent TCP callback. */
            s_start_do_now = 1;
            s_stream_requested = 1U;
            s_resume_stream_after_reconnect = 0U;
            UDP_LOG_INFO("ACTION START: ACK queued before UDP sampling");
        }
        return;
    }

    if (type == APP_TYPE_STOP)
    {
        (void)payload;
        (void)len;

        UDP_LOG_INFO("CMD STOP received");
        s_start_do_now = 0;
        s_stream_requested = 0;
        s_resume_stream_after_reconnect = 0;

        UDP_LOG_INFO("ACTION STOP: AdcStream_Stop");
        AdcStream_Stop();
        UDP_LOG_OK("ACTION STOP done: running=%u, ready=%u queued=%u flags=0x%08lX",
                   (unsigned int)AdcStream_IsRunning(),
                   (unsigned int)AdcStream_GetReadyCount(),
                   (unsigned int)AdcStream_GetQueuedCount(),
                   (unsigned long)AdcStream_GetFlags());

        /* STOP 后可能会把半包数据 flush 成 READY。ACK 必须排在所有数据之后。 */
        s_pending_stop_ack = 1;
        UDP_LOG_INFO("ACTION STOP: pending ACK after queued data drained");
        return;
    }


    if (type == APP_TYPE_GET_NET)
    {
        (void)payload;
        (void)len;

        UDP_LOG_INFO("CMD GET_NET received");
        (void)send_net_ack(0);
        return;
    }

    if (type == APP_TYPE_SET_NET)
    {
        AppNetConfig_t new_net;
        uint8_t flags;
        int32_t status = 0;

        UDP_LOG_INFO("CMD SET_NET received: payload_len=%u", (unsigned int)len);

        if (len < 8U)
        {
            UDP_LOG_ERR("CMD SET_NET invalid length: %u", (unsigned int)len);
            (void)send_net_ack(-30);
            return;
        }

        if (AdcStream_IsRunning())
        {
            UDP_LOG_ERR("CMD SET_NET rejected: stream is running, STOP first");
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

        UDP_LOG_INFO("CMD SET_NET params: server=%u.%u.%u.%u:%u, dev=%u, local=%u.%u.%u.%u, gw=%u.%u.%u.%u, reconnect=%lums, flags=0x%02X",
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
            UDP_LOG_ERR("CMD SET_NET rejected: invalid net params");
            (void)send_net_ack(-31);
            return;
        }

        g_app_net_cfg = new_net;

        if ((flags & APP_NET_SET_FLAG_SAVE) != 0U)
        {
            if (FlashParam_SaveNet(&g_app_net_cfg) != 0)
            {
                UDP_LOG_ERR("CMD SET_NET failed: FlashParam_SaveNet");
                status = -32;
            }
            else
            {
                UDP_LOG_OK("CMD SET_NET: saved to Flash");
            }
        }

        if (send_net_ack(status) == ERR_OK)
        {
            if ((status == 0) && ((flags & APP_NET_SET_FLAG_RECONNECT) != 0U))
            {
                s_reconnect_after_ack = 1U;
                UDP_LOG_INFO("CMD SET_NET: reopen UDP after queued NET_ACK is sent");
            }
        }
        return;
    }

    UDP_LOG_ERR("CMD UNKNOWN received: type=0x%02X len=%u", (unsigned int)type, (unsigned int)len);
    (void)send_ack(-99);
}



static uint8_t known_command(uint8_t type)
{
    return type == APP_TYPE_SET_PARAM || type == APP_TYPE_GET_PARAM ||
           type == APP_TYPE_START || type == APP_TYPE_STOP ||
           type == APP_TYPE_SET_NET || type == APP_TYPE_GET_NET;
}

static void on_udp_receive(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *address, u16_t port)
{
    CommandFrame *slot;
    (void)arg;
    if (!p) return;
    if (pcb != s_udp || port != pcb->remote_port ||
        !ip_addr_cmp(address, &pcb->remote_ip) ||
        p->tot_len > APP_UDP_COMMAND_MAX || s_rx_count == RX_QUEUE_LEN) {
        pbuf_free(p);
        return;
    }
    slot = &s_commands[(s_rx_head + s_rx_count) % RX_QUEUE_LEN];
    slot->len = pbuf_copy_partial(p, slot->data, p->tot_len, 0U);
    pbuf_free(p);
    if (!AdcUdp_ValidateCommand(slot->data, slot->len)) return;
    s_rx_count++;
}

static void close_session(void)
{
    if (s_stream_requested) s_resume_stream_after_reconnect = 1U;
    AdcStream_Stop();
    AdcStream_ResetDiscard();
    if (s_udp) { udp_remove(s_udp); s_udp = 0; }
    s_connected = s_start_do_now = s_reconnect_after_ack = 0U;
    s_ctrl_head = s_ctrl_count = s_rx_head = s_rx_count = 0U;
    s_data_len = s_data_offset = 0U;
    s_pending_stop_ack = 0;
    s_next_connect_tick = HAL_GetTick() + g_app_net_cfg.reconnect_ms;
}

static void open_session(void)
{
    ip_addr_t peer;
    ip4_addr_t local, mask, gateway;
    uint32_t now = HAL_GetTick();
    if ((int32_t)(now - s_next_connect_tick) < 0) return;
    if (!AppNetConfig_IsValid(&g_app_net_cfg)) g_app_net_cfg = AppNetConfig_Default();
    IP4_ADDR(&local, g_app_net_cfg.local_ip[0], g_app_net_cfg.local_ip[1],
             g_app_net_cfg.local_ip[2], g_app_net_cfg.local_ip[3]);
    IP4_ADDR(&mask, g_app_net_cfg.netmask[0], g_app_net_cfg.netmask[1],
             g_app_net_cfg.netmask[2], g_app_net_cfg.netmask[3]);
    IP4_ADDR(&gateway, g_app_net_cfg.gateway[0], g_app_net_cfg.gateway[1],
             g_app_net_cfg.gateway[2], g_app_net_cfg.gateway[3]);
    netif_set_addr(&gnetif, &local, &mask, &gateway);
    IP4_ADDR(&peer, g_app_net_cfg.server_ip[0], g_app_net_cfg.server_ip[1],
             g_app_net_cfg.server_ip[2], g_app_net_cfg.server_ip[3]);
    s_udp = udp_new();
    if (s_udp && udp_connect(s_udp, &peer, g_app_net_cfg.server_port) == ERR_OK) {
        udp_recv(s_udp, on_udp_receive, 0);
        s_last_rx_tick = s_last_register_tick = now;
        (void)send_register_frame();
        (void)send_ack(0);
        UDP_LOG_INFO("UDP registration: dev=%u server=%u.%u.%u.%u:%u local_port=%u",
                     g_app_net_cfg.device_id, g_app_net_cfg.server_ip[0],
                     g_app_net_cfg.server_ip[1], g_app_net_cfg.server_ip[2],
                     g_app_net_cfg.server_ip[3], g_app_net_cfg.server_port, s_udp->local_port);
    } else {
        if (s_udp) udp_remove(s_udp);
        s_udp = 0;
        s_next_connect_tick = now + g_app_net_cfg.reconnect_ms;
    }
}

void AdcTcpServer_Init(void)
{
    /* Keep the public API so ADC initialization and the main loop remain compatible. */
    AdcStream_Stop();
    s_stream_requested = s_resume_stream_after_reconnect = 0U;
    s_frame_seq = s_last_stat_tick = s_last_stat_seq = 0U;
    s_send_errors = s_sent_frames = s_sent_bytes = 0U;
    if (s_udp) udp_remove(s_udp);
    s_udp = 0;
    s_ctrl_head = s_ctrl_count = s_rx_head = s_rx_count = 0U;
    s_connected = s_start_do_now = s_reconnect_after_ack = 0U;
    s_data_len = s_data_offset = 0U;
    s_pending_stop_ack = 0;
    s_next_connect_tick = HAL_GetTick();
}

void AdcTcpServer_Task(void)
{
    uint32_t now = HAL_GetTick();
    uint16_t burst = 0U;
    AdcStreamBlock_t *block;
    AdcStream_Task();
    if (!netif_is_link_up(&gnetif) || !netif_is_up(&gnetif)) {
        if (s_udp) close_session();
        return;
    }
    if (!s_udp) { open_session(); return; }

    /* Register periodically, including while idle, to maintain NAT mappings. */
    if ((uint32_t)(now - s_last_register_tick) >= g_app_net_cfg.reconnect_ms) {
        if (send_register_frame() == ERR_OK) s_last_register_tick = now;
    }
    while (s_rx_count && s_ctrl_count < CTRL_QUEUE_LEN - 1U) {
        CommandFrame *command = &s_commands[s_rx_head];
        if (known_command(command->data[3])) {
            s_last_rx_tick = now;
            s_connected = 1U;
        }
        handle_frame(command->data[3], command->data + APP_FRAME_HDR_LEN,
                     (uint16_t)(command->len - 12U));
        s_rx_head = (uint8_t)((s_rx_head + 1U) % RX_QUEUE_LEN);
        s_rx_count--;
    }
    if ((uint32_t)(now - s_last_rx_tick) >= APP_UDP_PEER_TIMEOUT_MS) {
        UDP_LOG_ERR("Peer timeout: stop sampling and register again");
        close_session();
        return;
    }
    while (s_ctrl_count) {
        ControlFrame *control = &s_controls[s_ctrl_head];
        if (send_datagram(control->data, control->len) != ERR_OK) return;
        s_ctrl_head = (uint8_t)((s_ctrl_head + 1U) % CTRL_QUEUE_LEN);
        s_ctrl_count--;
    }
    if (s_reconnect_after_ack) { close_session(); return; }
    if (!s_connected) return;
    if (s_resume_stream_after_reconnect && s_stream_requested) s_start_do_now = 1U;
    if (s_start_do_now) {
        s_start_do_now = 0U;
        if (start_stream_now() != 0) (void)send_ack(-21);
    }

    /* A successful send only means acceptance by the local stack, never delivery. */
    while (burst < APP_UDP_SEND_BURST_MAX && (block = AdcStream_PeekReadyBlock()) != 0) {
        if (!s_data_len) {
            uint16_t count = AdcUdp_MaxSamples(gnetif.mtu, block->bytes_per_sample);
            uint16_t remaining;
            if (!count || block->data_bytes != (uint32_t)block->sample_count * block->bytes_per_sample ||
                s_data_offset >= block->sample_count) {
                UDP_LOG_ERR("Invalid ADC block or MTU");
                close_session();
                return;
            }
            remaining = (uint16_t)(block->sample_count - s_data_offset);
            if (count > remaining) count = remaining;
            s_data_count = count;
            s_data_len = AdcUdp_BuildData(s_data, sizeof(s_data), s_frame_seq++,
                &g_app_cfg, block->first_sample_seq + s_data_offset,
                block->frame + APP_FRAME_HDR_LEN + APP_DATA_META_LEN +
                    (uint32_t)s_data_offset * block->bytes_per_sample,
                count, block->bytes_per_sample);
            if (!s_data_len) { close_session(); return; }
        }
        if (send_datagram(s_data, s_data_len) != ERR_OK) break;
        s_sent_frames++;
        s_sent_bytes += (uint32_t)s_data_count * block->bytes_per_sample;
        s_data_offset = (uint16_t)(s_data_offset + s_data_count);
        s_data_len = 0U;
        if (s_data_offset == block->sample_count) {
            AdcStream_ReleaseReadyBlock();
            s_data_offset = 0U;
        }
        burst++;
        if ((burst & 1U) == 0U) AdcStream_Task();
    }
    if (s_pending_stop_ack && !AdcStream_GetReadyCount() && !s_data_len) {
        if (send_ack(s_pending_stop_ack > 0 ? 0 : s_pending_stop_ack) == ERR_OK)
            s_pending_stop_ack = 0;
    }
#if ADC_STREAM_STAT_LOG_ENABLE
    if ((uint32_t)(now - s_last_stat_tick) >= 1000U) {
        uint32_t elapsed = now - s_last_stat_tick;
        uint32_t sequence = AdcStream_GetSampleSeq();
        UDP_LOG_INFO("STAT fs=%lu actual=%lu run=%u adc=%lu/s seq=%lu datagrams=%lu bytes=%lu ready=%u free=%u ov=%lu send_err=%lu flags=0x%08lX",
            (unsigned long)g_app_cfg.fs_hz, (unsigned long)AppConfig_ActualFsHz(&g_app_cfg),
            AdcStream_IsRunning(), (unsigned long)(((uint64_t)(sequence - s_last_stat_seq) * 1000U) / elapsed),
            (unsigned long)sequence, (unsigned long)s_sent_frames, (unsigned long)s_sent_bytes,
            AdcStream_GetReadyCount(), AdcStream_GetFreeCount(),
            (unsigned long)AdcStream_GetOverrunCount(), (unsigned long)s_send_errors,
            (unsigned long)AdcStream_GetFlags());
        s_last_stat_tick = now;
        s_last_stat_seq = sequence;
    }
#endif
}
