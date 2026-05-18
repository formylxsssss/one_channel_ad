#include "ads127l11.h"
#include "spi.h"
#include <string.h>

extern SPI_HandleTypeDef hspi4;

#define ADS_REG_DEV_ID      0x00U
#define ADS_REG_STATUS      0x02U
#define ADS_REG_CONTROL     0x03U
#define ADS_REG_MUX         0x04U
#define ADS_REG_CONFIG1     0x05U
#define ADS_REG_CONFIG2     0x06U
#define ADS_REG_CONFIG3     0x07U
#define ADS_REG_CONFIG4     0x08U

#define ADS_CMD_NOP0        0x00U
#define ADS_CMD_RREG(a)     (uint8_t)(0x40U | ((a) & 0x0FU))
#define ADS_CMD_WREG(a)     (uint8_t)(0x80U | ((a) & 0x0FU))

#define ADS_SPI_BSY_TIMEOUT_LOOP        200000UL
#define ADS_SPI_FAST_TIMEOUT_LOOP       1200UL

static volatile uint32_t s_spi_timeout_count = 0;
static volatile uint32_t s_fast_read_start_count = 0;
static volatile uint32_t s_fast_read_ok_count = 0;
static volatile uint32_t s_fast_read_error_count = 0;
static volatile uint32_t s_dma_error_count = 0;

static inline void ads_cs_low(void)
{
    ADS127L11_CS_GPIO_Port->BSRR = ((uint32_t)ADS127L11_CS_Pin << 16U);
}

static inline void ads_cs_high(void)
{
    ADS127L11_CS_GPIO_Port->BSRR = (uint32_t)ADS127L11_CS_Pin;
}

static inline void ads_start_low(void)
{
    ADS127L11_START_GPIO_Port->BSRR = ((uint32_t)ADS127L11_START_Pin << 16U);
}

static inline void ads_start_high(void)
{
    ADS127L11_START_GPIO_Port->BSRR = (uint32_t)ADS127L11_START_Pin;
}

static int ads_wait_spi_not_busy(uint32_t loop_max)
{
    while ((SPI4->SR & SPI_SR_BSY) != 0U)
    {
        if (loop_max == 0U)
        {
            s_spi_timeout_count++;
            return -1;
        }
        loop_max--;
    }
    return 0;
}

static void ads_clear_spi_rx_ovr(void)
{
    volatile uint32_t tmp;
    uint32_t guard = 64U;

    while (((SPI4->SR & SPI_SR_RXNE) != 0U) && (guard > 0U))
    {
        tmp = SPI4->DR;
        (void)tmp;
        guard--;
    }

    tmp = SPI4->DR;
    tmp = SPI4->SR;
    (void)tmp;
}

static void ADS127L11_SPI4_ForceFastMode(void)
{
    /*
     * ADS127L11 SPI 使用 CPOL=0、CPHA=1。
     * APB2=84MHz 时，Prescaler=4，SPI4_SCK≈21MHz。
     * 400kSPS/24bit 每点 3 字节，需要约 9.6MHz，21MHz 余量较大。
     */
    if (hspi4.Instance != SPI4)
    {
        hspi4.Instance = SPI4;
    }

    (void)HAL_SPI_DeInit(&hspi4);

    hspi4.Init.Mode              = SPI_MODE_MASTER;
    hspi4.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi4.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi4.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi4.Init.CLKPhase          = SPI_PHASE_2EDGE;
    hspi4.Init.NSS               = SPI_NSS_SOFT;
    hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
    hspi4.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi4.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi4.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi4.Init.CRCPolynomial     = 10;

    (void)HAL_SPI_Init(&hspi4);
    __HAL_SPI_ENABLE(&hspi4);
}

void ADS127L11_BoardGpioInit(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_SPI4_CLK_ENABLE();

    /* 安全默认电平：RESET=1，START/SYNC=0，CS=1。 */
    HAL_GPIO_WritePin(ADS127L11_AD_RESET_GPIO_Port, ADS127L11_AD_RESET_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ADS127L11_START_GPIO_Port,    ADS127L11_START_Pin,    GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ADS127L11_CS_GPIO_Port,       ADS127L11_CS_Pin,       GPIO_PIN_SET);

    GPIO_InitStruct.Pin = ADS127L11_AD_RESET_Pin | ADS127L11_START_Pin | ADS127L11_CS_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /* SPI4: PE2=SCK, PE5=MISO, PE6=MOSI, AF5。 */
    GPIO_InitStruct.Pin = ADS127L11_SCK_Pin | ADS127L11_MISO_Pin | ADS127L11_MOSI_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI4;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /* DRDY: PE3，下降沿中断。 */
    GPIO_InitStruct.Pin = ADS127L11_DRDY_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    __HAL_GPIO_EXTI_CLEAR_IT(ADS127L11_DRDY_Pin);
    HAL_NVIC_SetPriority(EXTI3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI3_IRQn);
}

int ADS127L11_WriteReg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { ADS_CMD_WREG(reg), value };
    uint8_t rx[2] = {0, 0};

    ADS127L11_SPI4_ForceFastMode();
    ads_clear_spi_rx_ovr();
    ads_cs_low();

    if (HAL_SPI_TransmitReceive(&hspi4, tx, rx, 2, 100) != HAL_OK)
    {
        ads_cs_high();
        return -1;
    }

    if (ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP) != 0)
    {
        ads_cs_high();
        return -2;
    }

    ads_cs_high();
    ads_clear_spi_rx_ovr();
    return 0;
}

int ADS127L11_ReadReg(uint8_t reg, uint8_t *value)
{
    uint8_t tx1[2];
    uint8_t rx1[2] = {0, 0};
    uint8_t tx2[2] = { ADS_CMD_NOP0, ADS_CMD_NOP0 };
    uint8_t rx2[2] = {0, 0};

    if (value == 0)
    {
        return -1;
    }

    ADS127L11_SPI4_ForceFastMode();
    ads_clear_spi_rx_ovr();

    tx1[0] = ADS_CMD_RREG(reg);
    tx1[1] = 0x00U;

    ads_cs_low();
    if (HAL_SPI_TransmitReceive(&hspi4, tx1, rx1, 2, 100) != HAL_OK)
    {
        ads_cs_high();
        return -2;
    }
    if (ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP) != 0)
    {
        ads_cs_high();
        return -3;
    }
    ads_cs_high();

    ads_cs_low();
    if (HAL_SPI_TransmitReceive(&hspi4, tx2, rx2, 2, 100) != HAL_OK)
    {
        ads_cs_high();
        return -4;
    }
    if (ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP) != 0)
    {
        ads_cs_high();
        return -5;
    }
    ads_cs_high();

    ads_clear_spi_rx_ovr();
    *value = rx2[0];
    return 0;
}

int ADS127L11_Configure(const AppConfig_t *cfg)
{
    uint8_t config4;

    if (!AppConfig_IsValid(cfg))
    {
        return -1;
    }

    ADS127L11_Stop();
    ADS127L11_StreamExit();
    HAL_Delay(1);
    ADS127L11_SPI4_ForceFastMode();

    /* MUX: 正常输入极性。 */
    if (ADS127L11_WriteReg(ADS_REG_MUX, 0x00U) != 0) return -2;

    /* CONFIG1: REF_RNG=1；VCM 输出；AIN/REF 预充电缓冲打开。 */
    if (ADS127L11_WriteReg(ADS_REG_CONFIG1,
                           (uint8_t)((1U << 6) | (1U << 4) | (1U << 3) | (1U << 1) | (1U << 0))) != 0)
    {
        return -3;
    }

    /* CONFIG2: START/STOP 控制，高速模式，独立 DRDY 引脚。 */
    if (ADS127L11_WriteReg(ADS_REG_CONFIG2, 0x00U) != 0) return -4;

    /* CONFIG3: 根据 fs_hz 选择 wideband/OSR。 */
    if (ADS127L11_WriteReg(ADS_REG_CONFIG3, AppConfig_AdcFilterCode(cfg->fs_hz)) != 0) return -5;

    /* CONFIG4: DATA=1 输出 16bit，DATA=0 输出 24bit；关闭 STATUS/CRC，直接按 DRDY 读数据。 */
    config4 = (cfg->bits == APP_BITS_16) ? (uint8_t)(1U << 3) : 0x00U;
    if (ADS127L11_WriteReg(ADS_REG_CONFIG4, config4) != 0) return -6;

    return 0;
}

int ADS127L11_Init(const AppConfig_t *cfg)
{
    ADS127L11_BoardGpioInit();
    ADS127L11_SPI4_ForceFastMode();

    ads_cs_high();
    ads_start_low();

    HAL_GPIO_WritePin(ADS127L11_AD_RESET_GPIO_Port, ADS127L11_AD_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(2);
    HAL_GPIO_WritePin(ADS127L11_AD_RESET_GPIO_Port, ADS127L11_AD_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(3);

    ads_clear_spi_rx_ovr();
    return ADS127L11_Configure(cfg);
}

void ADS127L11_Start(void)
{
    ads_start_high();
}

void ADS127L11_Stop(void)
{
    ads_start_low();
}

void ADS127L11_StreamEnter(void)
{
    ADS127L11_SPI4_ForceFastMode();
    ads_clear_spi_rx_ovr();
    ads_cs_low();
}

void ADS127L11_StreamExit(void)
{
    (void)ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP);
    ads_cs_high();
    ads_clear_spi_rx_ovr();
}

static inline int spi4_xfer_fast(uint8_t out, uint8_t *in)
{
    uint32_t guard;

    if (in == 0)
    {
        return -1;
    }

    if ((SPI4->CR1 & SPI_CR1_SPE) == 0U)
    {
        s_spi_timeout_count++;
        return -2;
    }

    guard = ADS_SPI_FAST_TIMEOUT_LOOP;
    while ((SPI4->SR & SPI_SR_TXE) == 0U)
    {
        if (guard == 0U)
        {
            s_spi_timeout_count++;
            return -3;
        }
        guard--;
    }

    *(__IO uint8_t *)&SPI4->DR = out;

    guard = ADS_SPI_FAST_TIMEOUT_LOOP;
    while ((SPI4->SR & SPI_SR_RXNE) == 0U)
    {
        if (guard == 0U)
        {
            s_spi_timeout_count++;
            return -4;
        }
        guard--;
    }

    *in = *(__IO uint8_t *)&SPI4->DR;
    return 0;
}

int ADS127L11_ReadDataFast(uint8_t *dst, uint8_t bytes_per_sample)
{
    int ret;

    if (dst == 0)
    {
        return -1;
    }

    if ((bytes_per_sample != 2U) && (bytes_per_sample != 3U))
    {
        return -2;
    }

    s_fast_read_start_count++;

    ret = spi4_xfer_fast(ADS_CMD_NOP0, &dst[0]);
    if (ret != 0) { s_fast_read_error_count++; ads_clear_spi_rx_ovr(); return (int)(-10 + ret); }

    ret = spi4_xfer_fast(ADS_CMD_NOP0, &dst[1]);
    if (ret != 0) { s_fast_read_error_count++; ads_clear_spi_rx_ovr(); return (int)(-20 + ret); }

    if (bytes_per_sample == 3U)
    {
        ret = spi4_xfer_fast(ADS_CMD_NOP0, &dst[2]);
        if (ret != 0) { s_fast_read_error_count++; ads_clear_spi_rx_ovr(); return (int)(-30 + ret); }
    }

    s_fast_read_ok_count++;
    return 0;
}

/* 兼容旧 DMA 接口：本版本强制走寄存器快速读。 */
int ADS127L11_ReadDataDma(uint8_t *dst, uint8_t bytes_per_sample)
{
    return ADS127L11_ReadDataFast(dst, bytes_per_sample);
}

void ADS127L11_DmaCompleteFromIsr(void)
{
}

void ADS127L11_DmaErrorFromIsr(void)
{
    s_dma_error_count++;
}

uint8_t ADS127L11_IsDmaBusy(void)
{
    return 0U;
}

uint32_t ADS127L11_GetDmaStartCount(void)
{
    return s_fast_read_start_count;
}

uint32_t ADS127L11_GetDmaCompleteCount(void)
{
    return s_fast_read_ok_count;
}

uint32_t ADS127L11_GetDmaErrorCount(void)
{
    return s_fast_read_error_count + s_dma_error_count;
}

uint32_t ADS127L11_GetSpiTimeoutCount(void)
{
    return s_spi_timeout_count;
}

uint32_t ADS127L11_GetDmaInitError(void)
{
    return 0U;
}

uint32_t ADS127L11_GetLastHalSpiError(void)
{
    return 0U;
}

uint32_t ADS127L11_GetLastHalSpiState(void)
{
    return 0U;
}
