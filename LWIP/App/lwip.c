/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * File Name          : LWIP.c
  * Description        : This file provides initialization code for LWIP
  *                      middleWare.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#if defined ( __CC_ARM )  /* MDK ARM Compiler */
#include "lwip/sio.h"
#endif /* MDK ARM Compiler */
#include "ethernetif.h"

/* USER CODE BEGIN 0 */

/*
 * 说明：
 * 你的 main.c 已经通过 MDIO 自检确认：
 *   LAN8720A found
 *   link=UP
 *   an=DONE
 *   100M Full Duplex
 *
 * 但 Windows ping 显示“无法访问目标主机”，说明 LwIP 侧可能仍未把 netif link 置为 UP。
 *
 * 这里先强制 LwIP netif link up，避免 ethernet_link_check_state()
 * 因为 PHY 地址/驱动模板等问题把 link 状态覆盖为 down。
 *
 * 后面如果 ping/TCP 都稳定，可以把这个宏改成 0，恢复 CubeMX 默认周期链路检测。
 */
#define LWIP_FORCE_LINK_UP_FROM_PHY_SELFTEST        0

/* USER CODE END 0 */

/* Private function prototypes -----------------------------------------------*/
static void ethernet_link_status_updated(struct netif *netif);

#if (LWIP_FORCE_LINK_UP_FROM_PHY_SELFTEST == 0)
static void Ethernet_Link_Periodic_Handle(struct netif *netif);
#endif

/* ETH Variables initialization ----------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

uint32_t EthernetLinkTimer;

/* Variables Initialization */
struct netif gnetif;
ip4_addr_t ipaddr;
ip4_addr_t netmask;
ip4_addr_t gw;
uint8_t IP_ADDRESS[4];
uint8_t NETMASK_ADDRESS[4];
uint8_t GATEWAY_ADDRESS[4];

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

/**
  * LwIP initialization function
  */
void MX_LWIP_Init(void)
{
  struct netif *netif_ret;

  /* IP addresses initialization */
  IP_ADDRESS[0] = 192;
  IP_ADDRESS[1] = 168;
  IP_ADDRESS[2] = 1;
  IP_ADDRESS[3] = 100;

  NETMASK_ADDRESS[0] = 255;
  NETMASK_ADDRESS[1] = 255;
  NETMASK_ADDRESS[2] = 255;
  NETMASK_ADDRESS[3] = 0;

  GATEWAY_ADDRESS[0] = 192;
  GATEWAY_ADDRESS[1] = 168;
  GATEWAY_ADDRESS[2] = 1;
  GATEWAY_ADDRESS[3] = 1;

/* USER CODE BEGIN IP_ADDRESSES */
/* USER CODE END IP_ADDRESSES */

  /* Initialize the LwIP stack without RTOS */
  lwip_init();

  /* IP addresses initialization without DHCP (IPv4) */
  IP4_ADDR(&ipaddr,
           IP_ADDRESS[0],
           IP_ADDRESS[1],
           IP_ADDRESS[2],
           IP_ADDRESS[3]);

  IP4_ADDR(&netmask,
           NETMASK_ADDRESS[0],
           NETMASK_ADDRESS[1],
           NETMASK_ADDRESS[2],
           NETMASK_ADDRESS[3]);

  IP4_ADDR(&gw,
           GATEWAY_ADDRESS[0],
           GATEWAY_ADDRESS[1],
           GATEWAY_ADDRESS[2],
           GATEWAY_ADDRESS[3]);

  /*
   * Add the network interface.
   */
  netif_ret = netif_add(&gnetif,
                        &ipaddr,
                        &netmask,
                        &gw,
                        NULL,
                        &ethernetif_init,
                        &ethernet_input);

  if (netif_ret == NULL)
  {
    Error_Handler();
  }

  /*
   * Register the default network interface.
   */
  netif_set_default(&gnetif);

  /*
   * Set the link callback function.
   */
  netif_set_link_callback(&gnetif, ethernet_link_status_updated);

  /*
   * Bring the network interface administratively up.
   */
  netif_set_up(&gnetif);

#if (LWIP_FORCE_LINK_UP_FROM_PHY_SELFTEST != 0)

  /*
   * 当前调试阶段：
   * main.c 已经通过 LAN8720A 自检确认 PHY link 是 UP，
   * 所以这里强制告诉 LwIP：链路已连接。
   *
   * 这样 Windows 才能收到 ARP/ICMP 响应。
   */
  netif_set_link_up(&gnetif);

#else

  /*
   * CubeMX 默认方式：
   * 由 ethernet_link_check_state() 周期检测 PHY link 状态。
   */
  EthernetLinkTimer = HAL_GetTick();
  ethernet_link_check_state(&gnetif);

#endif

/* USER CODE BEGIN 3 */

/* USER CODE END 3 */
}

#ifdef USE_OBSOLETE_USER_CODE_SECTION_4
/* Kept to help code migration. (See new 4_1, 4_2... sections) */
/* Avoid to use this user section which will become obsolete. */
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
#endif

#if (LWIP_FORCE_LINK_UP_FROM_PHY_SELFTEST == 0)
/**
  * @brief  Ethernet Link periodic check
  * @param  netif
  * @retval None
  */
static void Ethernet_Link_Periodic_Handle(struct netif *netif)
{
/* USER CODE BEGIN 4_4_1 */
/* USER CODE END 4_4_1 */

  /* Ethernet Link every 100ms */
  if (HAL_GetTick() - EthernetLinkTimer >= 100U)
  {
    EthernetLinkTimer = HAL_GetTick();
    ethernet_link_check_state(netif);
  }

/* USER CODE BEGIN 4_4 */
/* USER CODE END 4_4 */
}
#endif

/**
 * ----------------------------------------------------------------------
 * Function given to help user to continue LwIP Initialization
 * Up to user to complete or change this function ...
 * Up to user to call this function in main.c in while (1) of main(void)
 *-----------------------------------------------------------------------
 * Read a received packet from the Ethernet buffers
 * Send it to the lwIP stack for handling
 * Handle timeouts if LWIP_TIMERS is set and without RTOS
 * Handle the link status if LWIP_NETIF_LINK_CALLBACK is set and without RTOS
 */
void MX_LWIP_Process(void)
{
/* USER CODE BEGIN 4_1 */
/* USER CODE END 4_1 */

  /*
   * 关键：
   * NO_SYS=1 裸机模式必须持续调用 ethernetif_input，
   * 否则板子收不到 ARP / ICMP / TCP 数据。
   */
  ethernetif_input(&gnetif);

/* USER CODE BEGIN 4_2 */
/* USER CODE END 4_2 */

  /*
   * Handle LwIP timeouts.
   */
  sys_check_timeouts();

#if (LWIP_FORCE_LINK_UP_FROM_PHY_SELFTEST == 0)
  Ethernet_Link_Periodic_Handle(&gnetif);
#else
  /*
   * 调试阶段保持 LwIP link up。
   * 避免默认链路检测把 netif link 改回 down。
   */
  if (!netif_is_link_up(&gnetif))
  {
    netif_set_link_up(&gnetif);
  }

  if (!netif_is_up(&gnetif))
  {
    netif_set_up(&gnetif);
  }
#endif

/* USER CODE BEGIN 4_3 */
/* USER CODE END 4_3 */
}

/**
  * @brief  Notify the User about the network interface link status
  * @param  netif: the network interface
  * @retval None
  */
static void ethernet_link_status_updated(struct netif *netif)
{
  /*
   * 注意：
   * 这里应该判断 netif_is_link_up()，
   * 不是判断 netif_is_up()。
   *
   * netif_is_up() 表示管理状态；
   * netif_is_link_up() 表示物理链路状态。
   */
  if (netif_is_link_up(netif))
  {
    netif_set_up(netif);

/* USER CODE BEGIN 5 */
/* USER CODE END 5 */
  }
  else
  {
    netif_set_down(netif);

/* USER CODE BEGIN 6 */
/* USER CODE END 6 */
  }
}

#if defined ( __CC_ARM )  /* MDK ARM Compiler */
/**
 * Opens a serial device for communication.
 *
 * @param devnum device number
 * @return handle to serial device if successful, NULL otherwise
 */
sio_fd_t sio_open(u8_t devnum)
{
  sio_fd_t sd;

/* USER CODE BEGIN 7 */
  sd = 0; /* dummy code */
/* USER CODE END 7 */

  return sd;
}

/**
 * Sends a single character to the serial device.
 *
 * @param c character to send
 * @param fd serial device handle
 *
 * @note This function will block until the character can be sent.
 */
void sio_send(u8_t c, sio_fd_t fd)
{
/* USER CODE BEGIN 8 */
  (void)c;
  (void)fd;
/* USER CODE END 8 */
}

/**
 * Reads from the serial device.
 *
 * @param fd pointer to data buffer for receiving
 * @param data pointer to data buffer for receiving
 * @param len maximum length in bytes to receive
 * @return number of bytes actually received
 */
u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 9 */
  (void)fd;
  (void)data;
  (void)len;
  recved_bytes = 0; /* dummy code */
/* USER CODE END 9 */

  return recved_bytes;
}

/**
 * Tries to read from the serial device.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length in bytes to receive
 * @return number of bytes actually received
 */
u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 10 */
  (void)fd;
  (void)data;
  (void)len;
  recved_bytes = 0; /* dummy code */
/* USER CODE END 10 */

  return recved_bytes;
}
#endif /* MDK ARM Compiler */
