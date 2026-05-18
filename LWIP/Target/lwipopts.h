/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : LwIP configuration for STM32F427VGT6 + LAN8720A
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WITH_RTOS                         0
#define CHECKSUM_BY_HARDWARE              1

#define NO_SYS                            1
#define SYS_LIGHTWEIGHT_PROT              0
#define MEM_ALIGNMENT                     4

/* RAM 安全版：比 CubeMX 默认快很多，但不会像 64KB 版本那样让 .bss 爆。 */
#define MEM_SIZE                          (32 * 1024)

#define LWIP_ETHERNET                     1
#define LWIP_ARP                          1
#define ARP_TABLE_SIZE                    10
#define ARP_QUEUEING                      1

/* 你的 ethernetif.c 使用 pbuf_custom / pbuf_alloced_custom，必须打开。 */
#define LWIP_SUPPORT_CUSTOM_PBUF          1

#define PBUF_POOL_SIZE                    16
#define PBUF_POOL_BUFSIZE                 1524

#define LWIP_IPV4                         1
#define LWIP_IPV6                         0
#define LWIP_ICMP                         1
#define LWIP_RAW                          1
#define LWIP_UDP                          1
#define LWIP_TCP                          1

#define LWIP_DHCP                         0
#define LWIP_DNS                          0
#define LWIP_AUTOIP                       0

#define LWIP_NETIF_STATUS_CALLBACK        1
#define LWIP_NETIF_LINK_CALLBACK          1

#define TCP_MSS                           1460
#define TCP_SND_BUF                       (4 * TCP_MSS)
#define TCP_WND                           (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                  16
#define TCP_SNDLOWAT                      (TCP_SND_BUF / 2)
#define TCP_SNDQUEUELOWAT                 4
#define TCP_WND_UPDATE_THRESHOLD          (TCP_WND / 4)
#define TCP_OVERSIZE                      TCP_MSS
#define TCP_QUEUE_OOSEQ                   0
#define LWIP_TCP_KEEPALIVE                0

#define MEMP_NUM_TCP_PCB                  4
#define MEMP_NUM_TCP_PCB_LISTEN           2
#define MEMP_NUM_TCP_SEG                  32
#define MEMP_NUM_PBUF                     16
#define MEMP_NUM_SYS_TIMEOUT              8

#define LWIP_NETCONN                      0
#define LWIP_SOCKET                       0

#define LWIP_TIMERS                       1
#define LWIP_TIMERS_CUSTOM                0

#define LWIP_STATS                        0
#define LWIP_DEBUG                        0

#define CHECKSUM_GEN_IP                   0
#define CHECKSUM_GEN_UDP                  0
#define CHECKSUM_GEN_TCP                  0
#define CHECKSUM_GEN_ICMP                 0
#define CHECKSUM_GEN_ICMP6                0
#define CHECKSUM_CHECK_IP                 0
#define CHECKSUM_CHECK_UDP                0
#define CHECKSUM_CHECK_TCP                0
#define CHECKSUM_CHECK_ICMP               0
#define CHECKSUM_CHECK_ICMP6              0

#define RECV_BUFSIZE_DEFAULT              2000000000
#define LWIP_DNS_SECURE                   7

#ifdef __cplusplus
}
#endif

#endif /* __LWIPOPTS__H__ */
