#include "main.h"
#include "ethernetif.h"
#include "SEGGER_RTT.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "selftest.h"
#include <string.h>

#define PC_PORT 5000U
#define RETRY_MS 3000U
#define TIMEOUT_MS 5000U
#define LOG(...) SEGGER_RTT_printf(0, __VA_ARGS__)

extern ETH_HandleTypeDef heth;
static struct netif demo_netif;
static struct tcp_pcb *client;
static ip_addr_t pc_ip;
static uint32_t phy_addr = 32U;
static uint32_t bsr, special, retry_tick, connect_tick, send_tick, pong_tick;
static uint32_t tx_ok, tx_fail, rx_frames, pong_count, unacked, ack_tick;
static uint32_t link_up_count, link_down_count;
static uint8_t connected;
static char rx_line[32];
static unsigned rx_used;

HAL_StatusTypeDef __real_HAL_ETH_Transmit(ETH_HandleTypeDef *, ETH_TxPacketConfig *, uint32_t);
HAL_StatusTypeDef __real_HAL_ETH_ReadData(ETH_HandleTypeDef *, void **);

HAL_StatusTypeDef __wrap_HAL_ETH_Transmit(ETH_HandleTypeDef *eth, ETH_TxPacketConfig *packet, uint32_t timeout)
{
    HAL_StatusTypeDef ret = __real_HAL_ETH_Transmit(eth, packet, timeout);
    if (ret == HAL_OK) tx_ok++; else tx_fail++;
    return ret;
}

HAL_StatusTypeDef __wrap_HAL_ETH_ReadData(ETH_HandleTypeDef *eth, void **packet)
{
    HAL_StatusTypeDef ret = __real_HAL_ETH_ReadData(eth, packet);
    if ((ret == HAL_OK) && (*packet != NULL)) rx_frames++;
    return ret;
}

void SysTick_Handler(void) { HAL_IncTick(); }

void Error_Handler(void)
{
    LOG("[DEMO FAIL] fatal: MACCR=%08X DMASR=%08X CFSR=%08X\r\n",
        ETH->MACCR, ETH->DMASR, SCB->CFSR);
    __disable_irq();
    for (;;) {}
}

static void clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 25;
    osc.PLL.PLLN = 336;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) Error_Handler();
}

static void board_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_ETH_CLK_ENABLE();
    SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_ETH;
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = ETH_RESET_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_RESET);
    HAL_GPIO_Init(ETH_RESET_GPIO_Port, &gpio);
    HAL_Delay(20);
    HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(300);
}

static void disconnect_client(void)
{
    struct tcp_pcb *old = client;
    client = NULL;
    connected = 0;
    unacked = 0;
    rx_used = 0;
    retry_tick = HAL_GetTick() + RETRY_MS;
    if (old != NULL)
    {
        tcp_arg(old, NULL);
        tcp_recv(old, NULL);
        tcp_sent(old, NULL);
        tcp_err(old, NULL);
        tcp_abort(old);
    }
}

static void link_task(void)
{
    static uint32_t last;
    uint32_t now = HAL_GetTick(), id1, id2, speed, duplex;
    ETH_MACConfigTypeDef mac;
    if ((now - last) < 100U) return;
    last = now;
    if (phy_addr >= 32U)
    {
        HAL_ETH_SetMDIOClockRange(&heth);
        for (phy_addr = 0; phy_addr < 32U; phy_addr++)
        {
            if ((HAL_ETH_ReadPHYRegister(&heth, phy_addr, 2, &id1) == HAL_OK) &&
                (HAL_ETH_ReadPHYRegister(&heth, phy_addr, 3, &id2) == HAL_OK) &&
                (id1 == 7U) && ((id2 & 0xFFF0U) == 0xC0F0U))
            {
                LOG("[PHY] addr=%u ID=%04X:%04X\r\n", phy_addr, id1, id2);
                break;
            }
        }
    }
    if ((phy_addr >= 32U) ||
        (HAL_ETH_ReadPHYRegister(&heth, phy_addr, 1, &bsr) != HAL_OK) ||
        (HAL_ETH_ReadPHYRegister(&heth, phy_addr, 1, &bsr) != HAL_OK) ||
        (HAL_ETH_ReadPHYRegister(&heth, phy_addr, 31, &special) != HAL_OK))
    {
        phy_addr = 32U;
        goto down;
    }
    if ((bsr & 0x0024U) != 0x0024U) goto down;
    switch ((special >> 2) & 7U)
    {
    case 1: speed = ETH_SPEED_10M; duplex = ETH_HALFDUPLEX_MODE; break;
    case 5: speed = ETH_SPEED_10M; duplex = ETH_FULLDUPLEX_MODE; break;
    case 2: speed = ETH_SPEED_100M; duplex = ETH_HALFDUPLEX_MODE; break;
    case 6: speed = ETH_SPEED_100M; duplex = ETH_FULLDUPLEX_MODE; break;
    default: goto down;
    }
    if (HAL_ETH_GetMACConfig(&heth, &mac) != HAL_OK) goto down;
    if ((heth.gState != HAL_ETH_STATE_STARTED) ||
        (mac.Speed != speed) || (mac.DuplexMode != duplex))
    {
        disconnect_client();
        if ((heth.gState == HAL_ETH_STATE_STARTED) && (HAL_ETH_Stop(&heth) != HAL_OK)) goto down;
        mac.Speed = speed;
        mac.DuplexMode = duplex;
        if ((HAL_ETH_SetMACConfig(&heth, &mac) != HAL_OK) ||
            (HAL_ETH_Start(&heth) != HAL_OK)) goto down;
        LOG("[PHY] link UP, MAC %uM %s duplex\r\n",
            speed == ETH_SPEED_100M ? 100U : 10U,
            duplex == ETH_FULLDUPLEX_MODE ? "full" : "half");
        retry_tick = now;
    }
    if (!netif_is_link_up(&demo_netif)) link_up_count++;
    netif_set_link_up(&demo_netif);
    netif_set_up(&demo_netif);
    return;
down:
    if (netif_is_link_up(&demo_netif))
    {
        link_down_count++;
        LOG("[PHY] link DOWN\r\n");
    }
    if (client != NULL) disconnect_client();
    if (heth.gState == HAL_ETH_STATE_STARTED) (void)HAL_ETH_Stop(&heth);
    netif_set_link_down(&demo_netif);
    netif_set_down(&demo_netif);
}

static void on_error(void *arg, err_t err)
{
    (void)arg;
    client = NULL; /* LwIP already freed this PCB. */
    connected = 0;
    unacked = 0;
    rx_used = 0;
    retry_tick = HAL_GetTick() + RETRY_MS;
    LOG("[TCP] error=%d, retry in %ums\r\n", (int)err, RETRY_MS);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t length)
{
    (void)arg; (void)pcb;
    unacked = (unacked >= length) ? unacked - length : 0;
    ack_tick = HAL_GetTick();
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if ((p == NULL) || (err != ERR_OK))
    {
        if (p != NULL) pbuf_free(p);
        LOG("[TCP] peer closed/error=%d\r\n", (int)err);
        disconnect_client();
        return ERR_ABRT;
    }
    tcp_recved(pcb, p->tot_len);
    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const char *data = q->payload;
        for (unsigned i = 0; i < q->len; i++)
        {
            char c = data[i];
            if (c == '\n')
            {
                rx_line[rx_used] = 0;
                if (strcmp(rx_line, "PC_PONG") == 0)
                {
                    pong_tick = HAL_GetTick();
                    pong_count++;
                    LOG("[RX OK] PC_PONG count=%u\r\n", pong_count);
                }
                rx_used = 0;
            }
            else if (rx_used < sizeof(rx_line) - 1) rx_line[rx_used++] = c;
            else rx_used = 0;
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK)
    {
        disconnect_client();
        return ERR_ABRT;
    }
    connected = 1;
    unacked = 0;
    rx_used = 0;
    pong_tick = ack_tick = HAL_GetTick();
    send_tick = pong_tick - 1000U;
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_nagle_disable(pcb);
    LOG("[TCP OK] connected to PC 192.168.11.1:%u\r\n", PC_PORT);
    return ERR_OK;
}

static void client_task(void)
{
    uint32_t now = HAL_GetTick();
    err_t err;
    if (!netif_is_link_up(&demo_netif) || !netif_is_up(&demo_netif)) return;
    if (client == NULL)
    {
        if ((int32_t)(now - retry_tick) < 0) return;
        client = tcp_new();
        if (client == NULL) { retry_tick = now + RETRY_MS; return; }
        tcp_err(client, on_error);
        connect_tick = now;
        LOG("[TCP] connect PC 192.168.11.1:%u\r\n", PC_PORT);
        err = tcp_connect(client, &pc_ip, PC_PORT, on_connected);
        if (err != ERR_OK) { LOG("[TCP] connect error=%d\r\n", (int)err); disconnect_client(); }
        return;
    }
    if (!connected)
    {
        if ((now - connect_tick) >= TIMEOUT_MS)
        {
            LOG("[TCP] connect timeout\r\n");
            disconnect_client();
        }
        return;
    }
    if (((unacked != 0) && ((now - ack_tick) >= TIMEOUT_MS)) ||
        ((now - pong_tick) >= TIMEOUT_MS))
    {
        LOG("[TCP] ACK/PONG timeout\r\n");
        disconnect_client();
        return;
    }
    if ((unacked == 0) && ((now - send_tick) >= 1000U))
    {
        static const char ping[] = "BOARD_PING\n";
        err = tcp_write(client, ping, sizeof(ping) - 1, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK)
        {
            unacked = sizeof(ping) - 1;
            ack_tick = send_tick = now;
            (void)tcp_output(client);
        }
        else LOG("[TCP] write error=%d\r\n", (int)err);
    }
}

int main(void)
{
    ip4_addr_t ip, mask, gw;
    uint32_t stat_tick = 0;
    SEGGER_RTT_Init();
    SEGGER_RTT_SetFlagsUpBuffer(0, SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    HAL_Init();
    clock_init();
    LOG("\r\n[DEMO] Ethernet-only TCP client, ADC disabled\r\n");
    board_init();
    IP4_ADDR(&ip, 192, 168, 11, 100);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    IP4_ADDR(&pc_ip, 192, 168, 11, 1);
    lwip_init();
    if (netif_add(&demo_netif, &ip, &mask, &gw, NULL, ethernetif_init, ethernet_input) == NULL) Error_Handler();
    netif_set_default(&demo_netif);
    EthDemo_Selftest(&demo_netif, &heth);
    tx_ok = tx_fail = rx_frames = 0; /* Normal-network counters exclude loopback frames. */
    LOG("[DEMO] board=192.168.11.100/24 PC=192.168.11.1:%u\r\n", PC_PORT);
    for (;;)
    {
        link_task();
        if (heth.gState == HAL_ETH_STATE_STARTED) ethernetif_input(&demo_netif);
        sys_check_timeouts();
        client_task();
        if ((HAL_GetTick() - stat_tick) >= 2000U)
        {
            stat_tick = HAL_GetTick();
            LOG("[STAT] ms=%u UP=%u DOWN=%u link=%u BSR=%04X PHY=%04X conn=%u TX=%u fail=%u RX=%u PONG=%u MACCR=%08X DMA=%08X\r\n",
                stat_tick, link_up_count, link_down_count,
                netif_is_link_up(&demo_netif), bsr, special, connected,
                tx_ok, tx_fail, rx_frames, pong_count, ETH->MACCR, ETH->DMASR);
        }
    }
}
