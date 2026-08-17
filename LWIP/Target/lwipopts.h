/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : LwIP configuration for STM32F427 + LAN8720A ADC TCP stream
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* STM32CubeMX Specific Parameters ------------------------------------------*/
#define WITH_RTOS                       0
#define CHECKSUM_BY_HARDWARE            1

/* Basic LwIP mode -----------------------------------------------------------*/
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define MEM_ALIGNMENT                   4
#define LWIP_ETHERNET                   1
#define LWIP_ARP                        1
#define LWIP_IPV4                       1
#define LWIP_ICMP                       1
#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_DNS_SECURE                 7
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_STATS                      0

/* Ethernet zero-copy/custom pbuf support.
 * 如果你的 ethernetif.c 中使用 struct pbuf_custom / pbuf_alloced_custom，
 * 这里必须打开，否则会出现 pbuf_custom incomplete type 编译错误。
 */
#define LWIP_SUPPORT_CUSTOM_PBUF        1

/* TCP streaming performance -------------------------------------------------
 * 400kSPS / 24bit 约 1.2MB/s，100M 全双工链路本身足够。
 * 真正要避免的是 tcp_write 因 send queue / tcp_seg 不足而短时停顿。
 */
#ifndef TCP_MSS
#define TCP_MSS                         1460
#endif

#define TCP_WND                         (4 * TCP_MSS)      /* 5840: RX命令很小，降低接收窗口省RAM */
#define TCP_SND_BUF                     (8 * TCP_MSS)      /* 11680 */
#define TCP_SND_QUEUELEN                48
#define TCP_SNDLOWAT                    (TCP_SND_BUF / 2)
#define TCP_SNDQUEUELOWAT               12
#define TCP_WND_UPDATE_THRESHOLD        (TCP_MSS * 2)

/* 这个必须大于 TCP_SND_QUEUELEN，否则 sndbuf 还有空间也可能发不出去。 */
#define MEMP_NUM_TCP_SEG                96
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         4

/* Heap / pbuf.
 * 本工程 TCP 发送 DATA 使用 tcp_write(..., flags=0) 零拷贝，
 * 发送大数据不主要消耗 MEM_SIZE；MEM_SIZE 主要用于少量 ACK/控制帧和 LwIP 内部结构。
 * 零拷贝发送需要 MEMP_NUM_PBUF 提供 PBUF_ROM 结构；如果这个值太小，
 * 会出现 sndbuf/sndq 看起来不满但 tcp_write 返回 ERR_MEM、memerr 持续增长。
 * 为了容纳 400k/24bit 使用的 4096-frame SPI DMA 缓冲，RX PBUF_POOL 仍保持较小。
 */
#define MEM_SIZE                        (16 * 1024)
#define PBUF_POOL_SIZE                  8
#define PBUF_POOL_BUFSIZE               1524
#define MEMP_NUM_PBUF                   96

/* Keepalive/recv defaults ---------------------------------------------------*/
#define RECV_BUFSIZE_DEFAULT            2000000000
#define TCP_QUEUE_OOSEQ                 0
#define TCP_OVERSIZE                    0
#define TCP_CALCULATE_EFF_SEND_MSS      0

/* Hardware checksum ---------------------------------------------------------*/
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_GEN_TCP                0
#define CHECKSUM_GEN_ICMP               0
#define CHECKSUM_GEN_ICMP6              0
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_CHECK_ICMP             0
#define CHECKSUM_CHECK_ICMP6            0

#ifdef __cplusplus
}
#endif

#endif /* __LWIPOPTS__H__ */
