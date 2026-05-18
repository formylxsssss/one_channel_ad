#ifndef ADS127L11_H
#define ADS127L11_H

#include "app_config.h"
#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 当前原理图固定连接：STM32F427VGT6 + ADS127L11
 *   PE0 -> AD_RESET
 *   PE1 -> SYNC/START
 *   PE2 -> SPI4_SCK
 *   PE3 -> DRDY
 *   PE4 -> SPI4_NSS/CS
 *   PE5 -> SPI4_MISO
 *   PE6 -> SPI4_MOSI
 *
 * 不依赖 CubeMX 的 User Label。
 */
#ifndef ADS127L11_AD_RESET_GPIO_Port
#define ADS127L11_AD_RESET_GPIO_Port     GPIOE
#endif
#ifndef ADS127L11_AD_RESET_Pin
#define ADS127L11_AD_RESET_Pin           GPIO_PIN_0
#endif

#ifndef ADS127L11_START_GPIO_Port
#define ADS127L11_START_GPIO_Port        GPIOE
#endif
#ifndef ADS127L11_START_Pin
#define ADS127L11_START_Pin              GPIO_PIN_1
#endif

#ifndef ADS127L11_SCK_GPIO_Port
#define ADS127L11_SCK_GPIO_Port          GPIOE
#endif
#ifndef ADS127L11_SCK_Pin
#define ADS127L11_SCK_Pin                GPIO_PIN_2
#endif

#ifndef ADS127L11_DRDY_GPIO_Port
#define ADS127L11_DRDY_GPIO_Port         GPIOE
#endif
#ifndef ADS127L11_DRDY_Pin
#define ADS127L11_DRDY_Pin               GPIO_PIN_3
#endif

#ifndef ADS127L11_CS_GPIO_Port
#define ADS127L11_CS_GPIO_Port           GPIOE
#endif
#ifndef ADS127L11_CS_Pin
#define ADS127L11_CS_Pin                 GPIO_PIN_4
#endif

#ifndef ADS127L11_MISO_GPIO_Port
#define ADS127L11_MISO_GPIO_Port         GPIOE
#endif
#ifndef ADS127L11_MISO_Pin
#define ADS127L11_MISO_Pin               GPIO_PIN_5
#endif

#ifndef ADS127L11_MOSI_GPIO_Port
#define ADS127L11_MOSI_GPIO_Port         GPIOE
#endif
#ifndef ADS127L11_MOSI_Pin
#define ADS127L11_MOSI_Pin               GPIO_PIN_6
#endif

void ADS127L11_BoardGpioInit(void);

int  ADS127L11_Init(const AppConfig_t *cfg);
int  ADS127L11_Configure(const AppConfig_t *cfg);
void ADS127L11_Start(void);
void ADS127L11_Stop(void);
void ADS127L11_StreamEnter(void);
void ADS127L11_StreamExit(void);
int  ADS127L11_ReadReg(uint8_t reg, uint8_t *value);
int  ADS127L11_WriteReg(uint8_t reg, uint8_t value);

/*
 * DRDY 中断内使用的 SPI4 寄存器快速读取函数。
 * StreamEnter() 会保持 CS 为低电平；本函数只发 NOP 产生 SCLK 并读取 2/3 字节数据。
 */
int ADS127L11_ReadDataFast(uint8_t *dst, uint8_t bytes_per_sample);

/* 兼容旧版本日志/接口：本寄存器版不再使用单样点 DMA。 */
int      ADS127L11_ReadDataDma(uint8_t *dst, uint8_t bytes_per_sample);
void     ADS127L11_DmaCompleteFromIsr(void);
void     ADS127L11_DmaErrorFromIsr(void);
uint8_t  ADS127L11_IsDmaBusy(void);
uint32_t ADS127L11_GetDmaStartCount(void);
uint32_t ADS127L11_GetDmaCompleteCount(void);
uint32_t ADS127L11_GetDmaErrorCount(void);
uint32_t ADS127L11_GetSpiTimeoutCount(void);
uint32_t ADS127L11_GetDmaInitError(void);
uint32_t ADS127L11_GetLastHalSpiError(void);
uint32_t ADS127L11_GetLastHalSpiState(void);

#ifdef __cplusplus
}
#endif

#endif
