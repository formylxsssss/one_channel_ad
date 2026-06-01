/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * STM32F427VGT6 + LAN8720A + ADS127L11
 * TCP/IP ADC acquisition program
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "lwip.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_config.h"
#include "app_net_config.h"
#include "flash_param.h"
#include "range_ctrl.h"
#include "ads127l11.h"
#include "adc_stream.h"
#include "adc_tcp_server.h"
#include "SEGGER_RTT.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

typedef struct
{
  uint8_t  found;
  uint8_t  phy_addr;
  uint16_t id1;
  uint16_t id2;
  uint16_t bcr;
  uint16_t bsr1;
  uint16_t bsr2;
  uint16_t anar;
  uint16_t anlpar;
  uint16_t aner;
  uint16_t special;
} EthPhyInfo_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define ETH_PHY_BOOT_WAIT_MS            5000U

#define ETH_PHY_RESET_LOW_MS            20U
#define ETH_PHY_RESET_RELEASE_MS        300U

#define LAN8720_REG_BCR                 0x00U
#define LAN8720_REG_BSR                 0x01U
#define LAN8720_REG_PHYID1              0x02U
#define LAN8720_REG_PHYID2              0x03U
#define LAN8720_REG_ANAR                0x04U
#define LAN8720_REG_ANLPAR              0x05U
#define LAN8720_REG_ANER                0x06U
#define LAN8720_REG_SPECIAL             0x1FU

#define LAN8720_BSR_AUTONEG_COMPLETE    (1U << 5)
#define LAN8720_BSR_LINK_STATUS         (1U << 2)

#define LAN8720_ID1_EXPECTED            0x0007U
#define LAN8720_ID2_MASK                0xFFF0U
#define LAN8720_ID2_EXPECTED_MASKED     0xC0F0U

#ifndef ETH_MACMIIAR_MB
#define ETH_MACMIIAR_MB                 0x00000001U
#endif

#ifndef ETH_MACMIIAR_CR_Div16
#define ETH_MACMIIAR_CR_Div16           0x00000008U
#endif

#ifndef ETH_MACMIIAR_CR_Div26
#define ETH_MACMIIAR_CR_Div26           0x0000000CU
#endif

#ifndef ETH_MACMIIAR_CR_Div42
#define ETH_MACMIIAR_CR_Div42           0x00000000U
#endif

#ifndef ETH_MACMIIAR_CR_Div62
#define ETH_MACMIIAR_CR_Div62           0x00000004U
#endif

#ifndef ETH_MACMIIAR_CR_Div102
#define ETH_MACMIIAR_CR_Div102          0x00000010U
#endif

#define ETH_MDIO_TIMEOUT_MS             100U
#define ETH_MACMIIAR_PA_SHIFT           11U
#define ETH_MACMIIAR_MR_SHIFT           6U
#define ETH_MACMIIAR_PA_MASK            0x0000F800U
#define ETH_MACMIIAR_MR_MASK            0x000007C0U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#ifndef myprintf
#define myprintf(...)                   SEGGER_RTT_printf(0, __VA_ARGS__)
#endif

#define LOG_OK(...)                     do { myprintf("[OK] ");   myprintf(__VA_ARGS__); myprintf("\r\n"); } while (0)
#define LOG_ERR(...)                    do { myprintf("[FAIL] "); myprintf(__VA_ARGS__); myprintf("\r\n"); } while (0)
#define LOG_INFO(...)                   do { myprintf("[INFO] "); myprintf(__VA_ARGS__); myprintf("\r\n"); } while (0)

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

extern struct netif gnetif;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN PFP */

static void DebugLog_Init(void);

static uint32_t Board_ETH_PHY_Reset(void);

static void ETH_Force_Clock_Enable(void);
static void ETH_Force_RMII_Mode(void);
static void ETH_Force_RMII_GPIO_Config(void);

static uint32_t ETH_Get_MDIO_CR(void);
static HAL_StatusTypeDef ETH_MDIO_Read(uint8_t phy_addr, uint8_t reg, uint16_t *value);

static const char *LAN8720_SpeedDuplexString(uint16_t special_reg);
static uint32_t LAN8720_ReadInfo(EthPhyInfo_t *info);
static uint32_t LAN8720_BootSelfTest(uint32_t wait_ms);

static void LwIP_ApplyNetConfig(const AppNetConfig_t *cfg);
static void LwIP_ForceNetifUp(void);
static void LwIP_PrintNetifState(void);
static void App_PrintConfig(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void DebugLog_Init(void)
{
  SEGGER_RTT_Init();

#ifdef SEGGER_RTT_MODE_NO_BLOCK_SKIP
  SEGGER_RTT_SetFlagsUpBuffer(0, SEGGER_RTT_MODE_NO_BLOCK_SKIP);
#endif

  myprintf("\r\n\r\n");
}

static uint32_t Board_ETH_PHY_Reset(void)
{
#ifdef ETH_RESET_Pin
  HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_RESET);
  HAL_Delay(ETH_PHY_RESET_LOW_MS);

  HAL_GPIO_WritePin(ETH_RESET_GPIO_Port, ETH_RESET_Pin, GPIO_PIN_SET);
  HAL_Delay(ETH_PHY_RESET_RELEASE_MS);

  return 0U;
#else
  return 1U;
#endif
}

static void ETH_Force_Clock_Enable(void)
{
#ifdef __HAL_RCC_ETH_CLK_ENABLE
  __HAL_RCC_ETH_CLK_ENABLE();
#endif

#ifdef __HAL_RCC_ETHMAC_CLK_ENABLE
  __HAL_RCC_ETHMAC_CLK_ENABLE();
#endif

#ifdef __HAL_RCC_ETHMACTX_CLK_ENABLE
  __HAL_RCC_ETHMACTX_CLK_ENABLE();
#endif

#ifdef __HAL_RCC_ETHMACRX_CLK_ENABLE
  __HAL_RCC_ETHMACRX_CLK_ENABLE();
#endif
}

static void ETH_Force_RMII_Mode(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();

#ifdef SYSCFG_PMC_MII_RMII_SEL
  SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
#endif
}

static void ETH_Force_RMII_GPIO_Config(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_NOPULL;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;

  /*
   * PA1 = ETH_RMII_REF_CLK
   * PA2 = ETH_MDIO
   * PA7 = ETH_RMII_CRS_DV
   */
  GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*
   * PB11 = ETH_RMII_TX_EN
   * PB12 = ETH_RMII_TXD0
   * PB13 = ETH_RMII_TXD1
   */
  GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*
   * PC1 = ETH_MDC
   * PC4 = ETH_RMII_RXD0
   * PC5 = ETH_RMII_RXD1
   */
  GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

static uint32_t ETH_Get_MDIO_CR(void)
{
  uint32_t hclk = HAL_RCC_GetHCLKFreq();

  /*
   * 当前 HCLK=168MHz，Div102 后 MDC 约 1.65MHz。
   */
  if (hclk >= 150000000U)
  {
    return ETH_MACMIIAR_CR_Div102;
  }
  else if (hclk >= 100000000U)
  {
    return ETH_MACMIIAR_CR_Div62;
  }
  else if (hclk >= 60000000U)
  {
    return ETH_MACMIIAR_CR_Div42;
  }
  else if (hclk >= 35000000U)
  {
    return ETH_MACMIIAR_CR_Div26;
  }
  else
  {
    return ETH_MACMIIAR_CR_Div16;
  }
}

static HAL_StatusTypeDef ETH_MDIO_Read(uint8_t phy_addr, uint8_t reg, uint16_t *value)
{
  uint32_t tickstart;
  uint32_t mdio_cr;

  if (value == NULL)
  {
    return HAL_ERROR;
  }

  mdio_cr = ETH_Get_MDIO_CR();

  tickstart = HAL_GetTick();
  while ((ETH->MACMIIAR & ETH_MACMIIAR_MB) != 0U)
  {
    if ((HAL_GetTick() - tickstart) > ETH_MDIO_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }

  /*
   * MW bit = 0，read。
   */
  ETH->MACMIIAR =
      ((((uint32_t)phy_addr) << ETH_MACMIIAR_PA_SHIFT) & ETH_MACMIIAR_PA_MASK) |
      ((((uint32_t)reg)      << ETH_MACMIIAR_MR_SHIFT) & ETH_MACMIIAR_MR_MASK) |
      mdio_cr |
      ETH_MACMIIAR_MB;

  tickstart = HAL_GetTick();
  while ((ETH->MACMIIAR & ETH_MACMIIAR_MB) != 0U)
  {
    if ((HAL_GetTick() - tickstart) > ETH_MDIO_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }

  *value = (uint16_t)(ETH->MACMIIDR & 0xFFFFU);
  return HAL_OK;
}

static const char *LAN8720_SpeedDuplexString(uint16_t special_reg)
{
  uint16_t hcdspeed = (uint16_t)((special_reg >> 2) & 0x07U);

  switch (hcdspeed)
  {
    case 0x01U:
      return "10M Half";

    case 0x05U:
      return "10M Full";

    case 0x02U:
      return "100M Half";

    case 0x06U:
      return "100M Full";

    default:
      return "Unknown";
  }
}

static uint32_t LAN8720_ReadInfo(EthPhyInfo_t *info)
{
  uint8_t addr;
  uint16_t id1;
  uint16_t id2;
  HAL_StatusTypeDef r1;
  HAL_StatusTypeDef r2;

  if (info == NULL)
  {
    return 1U;
  }

  memset(info, 0, sizeof(*info));

  for (addr = 0U; addr < 32U; addr++)
  {
    id1 = 0U;
    id2 = 0U;

    r1 = ETH_MDIO_Read(addr, LAN8720_REG_PHYID1, &id1);
    r2 = ETH_MDIO_Read(addr, LAN8720_REG_PHYID2, &id2);

    if ((r1 != HAL_OK) || (r2 != HAL_OK))
    {
      continue;
    }

    if (((id1 == 0x0000U) && (id2 == 0x0000U)) ||
        ((id1 == 0xFFFFU) && (id2 == 0xFFFFU)))
    {
      continue;
    }

    if ((id1 == LAN8720_ID1_EXPECTED) &&
        ((id2 & LAN8720_ID2_MASK) == LAN8720_ID2_EXPECTED_MASKED))
    {
      info->found = 1U;
      info->phy_addr = addr;
      info->id1 = id1;
      info->id2 = id2;

      (void)ETH_MDIO_Read(addr, LAN8720_REG_BCR,     &info->bcr);

      /*
       * BSR Link Status 是 latch-low，连续读两次。
       */
      (void)ETH_MDIO_Read(addr, LAN8720_REG_BSR,     &info->bsr1);
      (void)ETH_MDIO_Read(addr, LAN8720_REG_BSR,     &info->bsr2);

      (void)ETH_MDIO_Read(addr, LAN8720_REG_ANAR,    &info->anar);
      (void)ETH_MDIO_Read(addr, LAN8720_REG_ANLPAR,  &info->anlpar);
      (void)ETH_MDIO_Read(addr, LAN8720_REG_ANER,    &info->aner);
      (void)ETH_MDIO_Read(addr, LAN8720_REG_SPECIAL, &info->special);

      return 0U;
    }
  }

  return 2U;
}

static uint32_t LAN8720_BootSelfTest(uint32_t wait_ms)
{
  EthPhyInfo_t info;
  uint32_t start_tick;

  start_tick = HAL_GetTick();

  while ((HAL_GetTick() - start_tick) < wait_ms)
  {
    if (LAN8720_ReadInfo(&info) == 0U)
    {
      if (((info.bsr2 & LAN8720_BSR_LINK_STATUS) != 0U) &&
          ((info.bsr2 & LAN8720_BSR_AUTONEG_COMPLETE) != 0U))
      {
        LOG_OK("LAN8720A selftest: addr=%u, ID1=0x%04X, ID2=0x%04X, link=UP, speed=%s",
               info.phy_addr,
               info.id1,
               info.id2,
               LAN8720_SpeedDuplexString(info.special));
        return 0U;
      }
    }

    HAL_Delay(100);
  }

  if (LAN8720_ReadInfo(&info) == 0U)
  {
    LOG_ERR("LAN8720A selftest: PHY found but link not ready, addr=%u, BSR=0x%04X, SPECIAL=0x%04X, speed=%s",
            info.phy_addr,
            info.bsr2,
            info.special,
            LAN8720_SpeedDuplexString(info.special));
  }
  else
  {
    LOG_ERR("LAN8720A selftest: PHY not found by MDIO/MDC");
  }

  return 1U;
}

static void LwIP_ApplyNetConfig(const AppNetConfig_t *cfg)
{
  ip_addr_t ipaddr;
  ip_addr_t netmask;
  ip_addr_t gateway;

  if (cfg == NULL)
  {
    return;
  }

  IP4_ADDR(&ipaddr,
           cfg->local_ip[0],
           cfg->local_ip[1],
           cfg->local_ip[2],
           cfg->local_ip[3]);

  IP4_ADDR(&netmask,
           cfg->netmask[0],
           cfg->netmask[1],
           cfg->netmask[2],
           cfg->netmask[3]);

  IP4_ADDR(&gateway,
           cfg->gateway[0],
           cfg->gateway[1],
           cfg->gateway[2],
           cfg->gateway[3]);

  netif_set_addr(&gnetif, &ipaddr, &netmask, &gateway);

  LOG_OK("LwIP address apply: local=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u",
         cfg->local_ip[0], cfg->local_ip[1], cfg->local_ip[2], cfg->local_ip[3],
         cfg->netmask[0], cfg->netmask[1], cfg->netmask[2], cfg->netmask[3],
         cfg->gateway[0], cfg->gateway[1], cfg->gateway[2], cfg->gateway[3]);
}

static void LwIP_ForceNetifUp(void)
{
  if (!netif_is_up(&gnetif))
  {
    netif_set_up(&gnetif);
  }

  if (!netif_is_link_up(&gnetif))
  {
    netif_set_link_up(&gnetif);
  }
}

static void LwIP_PrintNetifState(void)
{
  LOG_OK("LwIP netif: up=%lu, link=%lu",
         netif_is_up(&gnetif) ? 1UL : 0UL,
         netif_is_link_up(&gnetif) ? 1UL : 0UL);
}

static void App_PrintConfig(void)
{
  LOG_OK("FlashParam load: fs=%lu, bits=%u, range=%u",
         (unsigned long)g_app_cfg.fs_hz,
         (unsigned int)g_app_cfg.bits,
         (unsigned int)g_app_cfg.range);

  LOG_OK("NetParam load: dev=%u, local=%u.%u.%u.%u, mask=%u.%u.%u.%u, gw=%u.%u.%u.%u, server=%u.%u.%u.%u:%u, reconnect=%lums",
         (unsigned int)g_app_net_cfg.device_id,
         g_app_net_cfg.local_ip[0], g_app_net_cfg.local_ip[1], g_app_net_cfg.local_ip[2], g_app_net_cfg.local_ip[3],
         g_app_net_cfg.netmask[0], g_app_net_cfg.netmask[1], g_app_net_cfg.netmask[2], g_app_net_cfg.netmask[3],
         g_app_net_cfg.gateway[0], g_app_net_cfg.gateway[1], g_app_net_cfg.gateway[2], g_app_net_cfg.gateway[3],
         g_app_net_cfg.server_ip[0], g_app_net_cfg.server_ip[1], g_app_net_cfg.server_ip[2], g_app_net_cfg.server_ip[3],
         (unsigned int)g_app_net_cfg.server_port,
         (unsigned long)g_app_net_cfg.reconnect_ms);
}

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void)
{
  HAL_StatusTypeDef eth_start_status;

  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  DebugLog_Init();
  LOG_INFO("BOOT");

  HAL_Init();
  LOG_OK("HAL_Init");

  SystemClock_Config();
  LOG_OK("SystemClock_Config");

  /*
   * GPIO 必须最先初始化。
   */
  MX_GPIO_Init();
  LOG_OK("GPIO init");

  /*
   * 只在这里强制一次 RMII/GPIO/ETH Clock。
   * 注意：LAN8720A 复位后不再重复执行 ETH_Force_RMII_Mode() / ETH_Force_Clock_Enable()。
   * 你当前程序卡在复位后的第二次强制动作，所以这里已经去掉。
   */
  ETH_Force_RMII_Mode();
  ETH_Force_RMII_GPIO_Config();
  ETH_Force_Clock_Enable();
  LOG_OK("ETH RMII GPIO/Clock force");

  if (Board_ETH_PHY_Reset() != 0U)
  {
    LOG_ERR("LAN8720A reset pin undefined: check ETH_RESET_Pin in main.h");
    Error_Handler();
  }
  LOG_OK("LAN8720A reset");

  MX_SPI4_Init();
  LOG_OK("SPI4 init");

  MX_TIM3_Init();
  LOG_OK("TIM3 init");

  /*
   * 从 STM32 内部 Flash 读取采样参数和 5G 主动连接参数。
   * 无有效参数时，FlashParam_LoadAll 内部会回退到默认值。
   * 注意：LwIP 初始化后还要把 g_app_net_cfg.local_ip/netmask/gateway 应用到 gnetif。
   */
  (void)FlashParam_LoadAll(&g_app_cfg, &g_app_net_cfg);
  App_PrintConfig();

  /*
   * 初始化 LwIP。
   * 如果程序卡在这里，最后一条日志会停在 NetParam load。
   */
  MX_LWIP_Init();
  LOG_OK("LwIP init");

  LwIP_ApplyNetConfig(&g_app_net_cfg);

  /*
   * 强制启动 ETH MAC/DMA。
   * 如果 HAL_BUSY，通常表示已经被 ethernetif 初始化流程启动过，这里继续运行。
   */
  eth_start_status = HAL_ETH_Start(&heth);
  if ((eth_start_status == HAL_OK) || (eth_start_status == HAL_BUSY))
  {
    LOG_OK("ETH MAC start, status=%d", eth_start_status);
  }
  else
  {
    LOG_ERR("ETH MAC start failed, status=%d", eth_start_status);
    Error_Handler();
  }

  LwIP_ForceNetifUp();
  LwIP_PrintNetifState();

  /*
   * 开机只自检一次。
   * 自检失败则停机，避免网络不可用时继续初始化采集业务。
   */
  if (LAN8720_BootSelfTest(ETH_PHY_BOOT_WAIT_MS) != 0U)
  {
    Error_Handler();
  }

  LwIP_ForceNetifUp();

  /*
   * 当前第一版程序默认 ADS127L11 使用内部时钟。
   * 所以这里不要启动 TIM3 PWM。
   *
   * 如果后面改为 ADS127L11 外部 CLK 模式，再打开：
   * HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
   */

  if (RangeCtrl_Set(g_app_cfg.range) != 0)
  {
    LOG_ERR("RangeCtrl set failed, range=%u", (unsigned int)g_app_cfg.range);
    Error_Handler();
  }
  LOG_OK("RangeCtrl set");

  AdcStream_Init();
  LOG_OK("AdcStream init");

  if (ADS127L11_Init(&g_app_cfg) != 0)
  {
    LOG_ERR("ADS127L11 init failed");
    Error_Handler();
  }
  LOG_OK("ADS127L11 init");

  AdcTcpServer_Init();
  LOG_OK("ADC TCP active client init");

  LOG_OK("Enter while(1)");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /*
     * NO_SYS=1 裸机 LwIP 必须循环调用。
     */
    MX_LWIP_Process();

    /*
     * 调试阶段保持 LwIP netif up/link up。
     */
    LwIP_ForceNetifUp();

    /*
     * TCP 命令处理和 ADC 数据发送任务。
     */
    AdcTcpServer_Task();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /*
   * HSE = 25MHz
   * PLLM = 25
   * PLLN = 336
   * PLLP = 2
   *
   * SYSCLK = 25 / 25 * 336 / 2 = 168MHz
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    LOG_ERR("HAL_RCC_OscConfig failed");
    Error_Handler();
  }

  /*
   * SYSCLK = 168MHz
   * AHB    = 168MHz
   * APB1   = 42MHz
   * APB2   = 84MHz
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
                                RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 |
                                RCC_CLOCKTYPE_PCLK2;

  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    LOG_ERR("HAL_RCC_ClockConfig failed");
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/*
 * 当前 main.c 逻辑：
 *
 * 1. RTT 非阻塞日志；
 * 2. HAL / SystemClock / GPIO 初始化；
 * 3. 只强制一次 RMII/GPIO/ETH Clock；
 * 4. 复位 LAN8720A；
 * 5. 复位后不再重复强制 RMII mode/clock；
 * 6. SPI4 / TIM3 / LwIP 初始化；
 * 7. HAL_ETH_Start；
 * 8. LAN8720A 开机自检一次；
 * 9. 自检通过后启动 ADS127L11 + TCP active client；
 * 10. while(1) 中持续 MX_LWIP_Process() 和 AdcTcpServer_Task()。
 */

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */

  SEGGER_RTT_Init();

#ifdef SEGGER_RTT_MODE_NO_BLOCK_SKIP
  SEGGER_RTT_SetFlagsUpBuffer(0, SEGGER_RTT_MODE_NO_BLOCK_SKIP);
#endif

  LOG_ERR("Error_Handler entered");
  LOG_ERR("RCC->CR=0x%08lX RCC->CFGR=0x%08lX RCC->PLLCFGR=0x%08lX",
          RCC->CR,
          RCC->CFGR,
          RCC->PLLCFGR);

  __disable_irq();

  while (1)
  {
  }

  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  LOG_ERR("ASSERT FAILED: file=%s line=%lu", file, (unsigned long)line);
  (void)file;
  (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */