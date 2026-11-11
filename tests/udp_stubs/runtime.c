#include "../../device/adc_tcp_server.c"

struct netif gnetif;
static struct udp_pcb pcb;
static struct pbuf outbound;
static uint8_t payload_buffer[APP_UDP_MAX_PAYLOAD];
static uint8_t packets[16][APP_UDP_MAX_PAYLOAD];
static uint16_t packet_lengths[16];
static uint32_t sent_count, now_ms, send_failures, port_counter, flash_saves;
void *_sbrk(int increment) { (void)increment; return (void *)-1; }

uint32_t HAL_GetTick(void) { return now_ms; }
int SEGGER_RTT_printf(unsigned channel, const char *format, ...)
{ (void)channel; (void)format; return 0; }
int ADS127L11_Configure(const AppConfig_t *cfg) { (void)cfg; return 0; }
int RangeCtrl_Set(uint8_t range) { (void)range; return 0; }
int FlashParam_Save(const AppConfig_t *cfg) { (void)cfg; flash_saves++; return 0; }
int FlashParam_SaveNet(const AppNetConfig_t *cfg) { (void)cfg; return 0; }
struct udp_pcb *udp_new(void) { memset(&pcb, 0, sizeof(pcb)); return &pcb; }
err_t udp_connect(struct udp_pcb *p, const ip_addr_t *ip, uint16_t port)
{ p->remote_ip = *ip; p->remote_port = port; p->local_port = (uint16_t)(49000 + ++port_counter); return ERR_OK; }
void udp_recv(struct udp_pcb *p, udp_recv_fn fn, void *arg) { (void)arg; p->recv = fn; }
void udp_remove(struct udp_pcb *p) { (void)p; }
struct pbuf *pbuf_alloc(int layer, uint16_t len, int kind)
{
    (void)layer; (void)kind;
    if (len > sizeof(payload_buffer)) return 0;
    outbound.payload = payload_buffer;
    outbound.len = outbound.tot_len = len;
    return &outbound;
}
err_t pbuf_take(struct pbuf *p, const void *data, uint16_t len)
{ memcpy(p->payload, data, len); return ERR_OK; }
uint16_t pbuf_copy_partial(const struct pbuf *p, void *out, uint16_t len, uint16_t offset)
{ memcpy(out, (const uint8_t *)p->payload + offset, len); return len; }
uint8_t pbuf_free(struct pbuf *p) { (void)p; return 1; }
err_t udp_send(struct udp_pcb *p, struct pbuf *b)
{
    (void)p;
    if (send_failures) { send_failures--; return ERR_MEM; }
    memcpy(packets[sent_count % 16U], b->payload, b->len);
    packet_lengths[sent_count % 16U] = b->len;
    sent_count++;
    return ERR_OK;
}
void netif_set_addr(struct netif *n, const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gateway)
{ n->ip = *ip; n->mask = *mask; n->gateway = *gateway; }
void TestReset(void)
{
    now_ms = sent_count = send_failures = flash_saves = 0U;
    g_app_cfg = AppConfig_Default();
    g_app_net_cfg = AppNetConfig_Default();
    gnetif.mtu = 1280;
    gnetif.up = gnetif.link = 1;
    AdcStream_Init();
    AdcTcpServer_Init();
}
void TestTick(uint32_t value) { now_ms = value; }
void TestLink(uint32_t value) { gnetif.link = (uint8_t)value; }
void TestFailSend(uint32_t value) { send_failures = value; }
void TestClearSent(void) { sent_count = 0U; }
uint32_t TestSentCount(void) { return sent_count; }
uint32_t TestConnected(void) { return s_connected; }
uint32_t TestFlashSaves(void) { return flash_saves; }
uint32_t TestCopySent(uint32_t index, uint8_t *out)
{ memcpy(out, packets[index % 16U], packet_lengths[index % 16U]); return packet_lengths[index % 16U]; }
void TestReceive(const uint8_t *data, uint32_t len)
{
    struct pbuf p = {(void *)data, (uint16_t)len, (uint16_t)len};
    if (pcb.recv) pcb.recv(0, &pcb, &p, &pcb.remote_ip, pcb.remote_port);
}
void TestWrongPeer(const uint8_t *data, uint32_t len)
{
    ip_addr_t wrong = {pcb.remote_ip.addr ^ 1U};
    struct pbuf p = {(void *)data, (uint16_t)len, (uint16_t)len};
    if (pcb.recv) pcb.recv(0, &pcb, &p, &wrong, pcb.remote_port);
}
