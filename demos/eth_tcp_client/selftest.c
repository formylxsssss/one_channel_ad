#include "selftest.h"
#include "SEGGER_RTT.h"
#include <string.h>

#define TEST_FRAMES 8U
volatile EthLoopReport g_eth_loop_report;

static int find_phy(ETH_HandleTypeDef *eth)
{
    uint32_t id1, id2;
    HAL_ETH_SetMDIOClockRange(eth);
    for (uint32_t addr = 0; addr < 32; addr++)
    {
        if ((HAL_ETH_ReadPHYRegister(eth, addr, 2, &id1) == HAL_OK) &&
            (HAL_ETH_ReadPHYRegister(eth, addr, 3, &id2) == HAL_OK) &&
            (id1 == 7U) && ((id2 & 0xFFF0U) == 0xC0F0U)) return (int)addr;
    }
    return -1;
}

static void run_stage(struct netif *netif, ETH_HandleTypeDef *eth,
                      uint32_t addr, unsigned stage, uint32_t bcr,
                      uint32_t speed, FunctionalState mac_loop,
                      const ETH_MACConfigTypeDef *saved)
{
    ETH_MACConfigTypeDef mac = *saved;
    volatile EthLoopResult *result = &g_eth_loop_report.stage[stage];
    ETH_TxPacketConfig packet = {0};
    ETH_BufferTypeDef buffer = {0};
    uint8_t frame[60] = {0};
    uint8_t received[60];
    uint32_t readback;

    if ((eth->gState == HAL_ETH_STATE_STARTED) && (HAL_ETH_Stop(eth) != HAL_OK)) goto failed;
    if (HAL_ETH_WritePHYRegister(eth, addr, 0, bcr) != HAL_OK) goto failed;
    HAL_Delay(150);
    if (HAL_ETH_ReadPHYRegister(eth, addr, 0, &readback) != HAL_OK) goto failed;
    result->phy_bcr = readback;
    if ((readback & 0x7100U) != (bcr & 0x7100U)) goto failed;
    mac.Speed = speed;
    mac.DuplexMode = ETH_FULLDUPLEX_MODE;
    mac.LoopbackMode = mac_loop;
    if ((HAL_ETH_SetMACConfig(eth, &mac) != HAL_OK) ||
        (HAL_ETH_Start(eth) != HAL_OK)) goto failed;

    memcpy(frame, netif->hwaddr, 6);
    memcpy(frame + 6, netif->hwaddr, 6);
    frame[12] = 0x88;
    frame[13] = 0xB5;
    memcpy(frame + 14, "RMII_TEST", 9);
    frame[23] = (uint8_t)stage;
    buffer.buffer = frame;
    buffer.len = sizeof(frame);
    packet.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
    packet.CRCPadCtrl = ETH_CRC_PAD_INSERT;
    packet.Length = sizeof(frame);
    packet.TxBuffer = &buffer;

    for (unsigned seq = 0; seq < TEST_FRAMES; seq++)
    {
        uint8_t matched = 0;
        frame[24] = (uint8_t)seq;
        if (HAL_ETH_Transmit(eth, &packet, 100) == HAL_OK) result->tx_ok++;
        else result->tx_fail++;
        uint32_t start = HAL_GetTick();
        do
        {
            struct pbuf *p = NULL;
            if ((HAL_ETH_ReadData(eth, (void **)&p) == HAL_OK) && (p != NULL))
            {
                uint16_t copied = pbuf_copy_partial(p, received, sizeof(received), 0);
                if ((copied >= 25) && (memcmp(received, frame, 25) == 0) && !matched)
                {
                    result->rx_ok++;
                    matched = 1;
                }
                else result->rx_other++;
                pbuf_free(p);
            }
        } while (!matched && ((HAL_GetTick() - start) < 60U));
    }
    goto finish;
failed:
    result->hal_error = eth->ErrorCode | 0x80000000U;
finish:
    result->hal_error |= eth->ErrorCode;
    result->maccr = eth->Instance->MACCR;
    result->dmasr = eth->Instance->DMASR;
    SEGGER_RTT_printf(0, "[LOOP] stage=%u TX=%u/%u RX_MATCH=%u/%u BCR=%04X ERR=%08X\r\n",
                      stage, result->tx_ok, TEST_FRAMES, result->rx_ok,
                      TEST_FRAMES, result->phy_bcr, result->hal_error);
    if (eth->gState == HAL_ETH_STATE_STARTED) (void)HAL_ETH_Stop(eth);
}

void EthDemo_Selftest(struct netif *netif, ETH_HandleTypeDef *eth)
{
    ETH_MACConfigTypeDef saved;
    uint32_t original_bcr;
    int addr;
    memset((void *)&g_eth_loop_report, 0, sizeof(g_eth_loop_report));
    g_eth_loop_report.magic = 0x4C425431U;
    addr = find_phy(eth);
    if ((addr < 0) || (HAL_ETH_GetMACConfig(eth, &saved) != HAL_OK) ||
        (HAL_ETH_ReadPHYRegister(eth, (uint32_t)addr, 0, &original_bcr) != HAL_OK))
    {
        g_eth_loop_report.restore_error = 0xFFFFFFFFU;
        g_eth_loop_report.done = 1;
        SEGGER_RTT_WriteString(0, "[LOOP] setup failed: PHY/MDIO unavailable\r\n");
        return;
    }
    g_eth_loop_report.phy_addr = (uint32_t)addr;
    g_eth_loop_report.original_bcr = original_bcr;
    SEGGER_RTT_WriteString(0, "[LOOP] stages: 0=MAC100 1=PHY100 2=PHY10\r\n");
    run_stage(netif, eth, (uint32_t)addr, 0, 0x2100U, ETH_SPEED_100M, ENABLE, &saved);
    run_stage(netif, eth, (uint32_t)addr, 1, 0x6100U, ETH_SPEED_100M, DISABLE, &saved);
    run_stage(netif, eth, (uint32_t)addr, 2, 0x4100U, ETH_SPEED_10M, DISABLE, &saved);
    saved.LoopbackMode = DISABLE;
    if (eth->gState == HAL_ETH_STATE_STARTED) (void)HAL_ETH_Stop(eth);
    if (HAL_ETH_SetMACConfig(eth, &saved) != HAL_OK) g_eth_loop_report.restore_error |= 1U;
    if ((original_bcr & 0x1000U) != 0) original_bcr |= 0x0200U;
    if (HAL_ETH_WritePHYRegister(eth, (uint32_t)addr, 0, original_bcr) != HAL_OK)
        g_eth_loop_report.restore_error |= 2U;
    g_eth_loop_report.done = 1;
    SEGGER_RTT_printf(0, "[LOOP] normal mode restored, error=%u\r\n", g_eth_loop_report.restore_error);
}
