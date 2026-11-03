#ifndef ETH_DEMO_SELFTEST_H
#define ETH_DEMO_SELFTEST_H

#include "ethernetif.h"
#include "main.h"

typedef struct
{
    uint32_t tx_ok, tx_fail, rx_ok, rx_other;
    uint32_t phy_bcr, maccr, dmasr, hal_error;
} EthLoopResult;

typedef struct
{
    uint32_t magic, done, phy_addr, original_bcr, restore_error;
    EthLoopResult stage[3];
} EthLoopReport;

extern volatile EthLoopReport g_eth_loop_report;
void EthDemo_Selftest(struct netif *netif, ETH_HandleTypeDef *eth);

#endif
