#include "ads127l11.h"
#include "adc_stream.h"
#include "spi.h"
#include "SEGGER_RTT.h"
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
#define ADS_SPI_FAST_TIMEOUT_LOOP       3000UL
#define SPI4_DMA_CHSEL                  (4UL << 25U)  /* DMA channel 4 */

#if defined(__GNUC__)
#define ADS_ALIGNED4 __attribute__((aligned(4)))
#else
#define ADS_ALIGNED4
#endif

static volatile uint32_t s_spi_timeout_count = 0;
static volatile uint32_t s_dma_start_count = 0;
static volatile uint32_t s_dma_irq_count = 0;
static volatile uint32_t s_dma_error_count = 0;
static volatile uint32_t s_dma_feif_count = 0;
static volatile uint32_t s_dma_fatal_count = 0;
static volatile uint32_t s_dma_init_error = 0;
static volatile uint32_t s_dma_pending_drop_count = 0;
static volatile uint8_t  s_dma_half_pending = 0;
static volatile uint8_t  s_dma_full_pending = 0;
static volatile uint32_t s_last_hal_spi_error = 0;
static volatile uint32_t s_last_hal_spi_state = 0;
static volatile uint32_t s_3wire_reset_count = 0;

static volatile uint8_t  s_cont_dma_running = 0;
static volatile uint8_t  s_cont_frame_bytes = 4;
static volatile uint8_t  s_cont_data_offset = 1;
static volatile uint8_t  s_cont_bps = 3;

/*
 * ADS127L11 3-wire 模式没有 CS 分帧，寄存器命令帧长度必须与当前输出帧长度一致。
 * 复位后默认输出为 24bit，无 STATUS/CRC，所以先按 3 字节帧配置寄存器。
 */
static volatile uint8_t  s_reg_frame_bytes = 3;
static volatile uint16_t s_cont_half_bytes = (uint16_t)(APP_ADC_CONT_DMA_FRAMES_PER_HALF * APP_ADC_CONT_MAX_FRAME_BYTES);
static volatile uint16_t s_cont_total_bytes = (uint16_t)(APP_ADC_CONT_DMA_TOTAL_FRAMES * APP_ADC_CONT_MAX_FRAME_BYTES);
static uint8_t ADS_ALIGNED4 s_dma_rx[APP_ADC_CONT_DMA_MAX_BYTES];
static uint8_t ADS_ALIGNED4 s_dma_dummy = 0x00U;

static inline void ads_cs_low(void)
{
    ADS127L11_CS_GPIO_Port->BSRR = ((uint32_t)ADS127L11_CS_Pin << 16U);
}

static inline void ads_cs_high_for_debug_only(void)
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

static uint32_t ads_spi_prescaler_for_cfg(const AppConfig_t *cfg)
{
    uint32_t required_hz;
    uint32_t frame_bits;
    uint32_t fs;

    /*
     * SPI4 在 APB2 上，当前工程 APB2=84MHz。
     * 连续 DMA 方案下，SPI 只是高速轮询 ADS127L11 输出帧；
     * 真正的新样点必须靠 STATUS.DRDY(bit0) 判断。
     *
     * 分频选择原则：SCLK 必须 >= fs * 输出帧bit数，且不要太离谱。
     * 这样不会读不完当前转换数据，也不会把旧帧误算成采样率。
     */
    if (cfg == 0)
    {
        return SPI_BAUDRATEPRESCALER_4;
    }

    fs = cfg->fs_hz;
    frame_bits = (uint32_t)AppConfig_BytesPerSample(cfg) * 8UL;
#if (APP_ADC_CONT_STATUS_ENABLE != 0U)
    frame_bits += 8UL;
#endif
    required_hz = fs * frame_bits;

    if (required_hz > 10500000UL) { return SPI_BAUDRATEPRESCALER_4;   } /* 21.000MHz */
    if (required_hz >  5250000UL) { return SPI_BAUDRATEPRESCALER_8;   } /* 10.500MHz */
    if (required_hz >  2625000UL) { return SPI_BAUDRATEPRESCALER_16;  } /* 5.250MHz */
    if (required_hz >  1312500UL) { return SPI_BAUDRATEPRESCALER_32;  } /* 2.625MHz */
    if (required_hz >   656250UL) { return SPI_BAUDRATEPRESCALER_64;  } /* 1.3125MHz */
    if (required_hz >   328125UL) { return SPI_BAUDRATEPRESCALER_128; } /* 0.65625MHz */
    return SPI_BAUDRATEPRESCALER_256;                                  /* 0.328125MHz */
}

static void ADS127L11_SPI4_ForceMode(uint32_t prescaler)
{
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
    hspi4.Init.BaudRatePrescaler = prescaler;
    hspi4.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi4.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi4.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi4.Init.CRCPolynomial     = 10;

    if (HAL_SPI_Init(&hspi4) != HAL_OK)
    {
        s_last_hal_spi_error = HAL_SPI_GetError(&hspi4);
        s_last_hal_spi_state = HAL_SPI_GetState(&hspi4);
    }

    __HAL_SPI_ENABLE(&hspi4);
}

static void ADS127L11_SPI4_ForceConfigMode(void)
{
    /* 寄存器配置阶段用 21MHz。 */
    ADS127L11_SPI4_ForceMode(SPI_BAUDRATEPRESCALER_4);
}

/*
 * ADS127L11 3-wire frame reset pattern：至少 63 个连续 1 后接 1 个 0。
 * 这里发送 64 bit：FF FF FF FF FF FF FF FE。
 * 作用：在 CS 始终为低的 3-wire 模式下，把 SPI 串行帧重新对齐。
 */
static int ads_3wire_frame_reset_pattern(void)
{
    uint8_t tx[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFEU};
    uint8_t rx[8] = {0};

    ads_cs_low();
    ads_clear_spi_rx_ovr();

    if (HAL_SPI_TransmitReceive(&hspi4, tx, rx, sizeof(tx), 100) != HAL_OK)
    {
        s_last_hal_spi_error = HAL_SPI_GetError(&hspi4);
        s_last_hal_spi_state = HAL_SPI_GetState(&hspi4);
        return -1;
    }

    if (ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP) != 0)
    {
        return -2;
    }

    ads_clear_spi_rx_ovr();
    s_3wire_reset_count++;
    return 0;
}

static uint8_t ads_current_output_frame_bytes_from_cfg(const AppConfig_t *cfg)
{
    uint8_t bps;

    if ((cfg != 0) && (cfg->bits == APP_BITS_16))
    {
        bps = 2U;
    }
    else
    {
        bps = 3U;
    }

#if (APP_ADC_CONT_STATUS_ENABLE != 0U)
    return (uint8_t)(bps + 1U);
#else
    return bps;
#endif
}

static int ads_3wire_xfer_cmd_frame(uint8_t cmd0, uint8_t cmd1, uint8_t *rx, uint8_t frame_bytes)
{
    uint8_t tx[5] = {0U, 0U, 0U, 0U, 0U};
    uint8_t rb[5] = {0U, 0U, 0U, 0U, 0U};

    if ((frame_bytes < 2U) || (frame_bytes > 5U))
    {
        return -1;
    }

    /* 手册要求：3-wire 下输入帧长度等于输出帧长度；命令在最后 16bit。 */
    tx[frame_bytes - 2U] = cmd0;
    tx[frame_bytes - 1U] = cmd1;

    ads_cs_low();

    if (HAL_SPI_TransmitReceive(&hspi4, tx, rb, frame_bytes, 100) != HAL_OK)
    {
        s_last_hal_spi_error = HAL_SPI_GetError(&hspi4);
        s_last_hal_spi_state = HAL_SPI_GetState(&hspi4);
        return -2;
    }

    if (ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP) != 0)
    {
        return -3;
    }

    ads_clear_spi_rx_ovr();

    if (rx != 0)
    {
        uint8_t i;
        for (i = 0U; i < frame_bytes; i++)
        {
            rx[i] = rb[i];
        }
    }

    return 0;
}

void ADS127L11_BoardGpioInit(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_SPI4_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    /*
     * 关键点：CS 必须在 ADS127L11 复位释放前保持低电平，
     * 让 ADC 进入 3-wire 模式。3-wire 模式下禁止再把 CS 拉高，
     * 否则会退出 3-wire，连续 DMA 会只读到第一帧/大量旧帧。
     */
    HAL_GPIO_WritePin(ADS127L11_AD_RESET_GPIO_Port, ADS127L11_AD_RESET_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ADS127L11_START_GPIO_Port,    ADS127L11_START_Pin,    GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ADS127L11_CS_GPIO_Port,       ADS127L11_CS_Pin,       GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = ADS127L11_AD_RESET_Pin | ADS127L11_START_Pin | ADS127L11_CS_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = ADS127L11_SCK_Pin | ADS127L11_MISO_Pin | ADS127L11_MOSI_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI4;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = ADS127L11_DRDY_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    __HAL_GPIO_EXTI_CLEAR_IT(ADS127L11_DRDY_Pin);
    HAL_NVIC_SetPriority(EXTI3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI3_IRQn);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 1, 0); /* SPI4_RX */
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

    HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 1, 1); /* SPI4_TX */
    HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
}

int ADS127L11_WriteReg(uint8_t reg, uint8_t value)
{
    int ret;
    uint8_t frame_bytes = s_reg_frame_bytes;

    ADS127L11_StopContinuousDma();
    ADS127L11_SPI4_ForceConfigMode();

    /* 在 3-wire 模式下，每次进入寄存器操作前先对齐串行帧。 */
    ret = ads_3wire_frame_reset_pattern();
    if (ret != 0)
    {
        return -10 + ret;
    }

    ret = ads_3wire_xfer_cmd_frame(ADS_CMD_WREG(reg), value, 0, frame_bytes);
    if (ret != 0)
    {
        return -20 + ret;
    }

    return 0;
}

int ADS127L11_ReadReg(uint8_t reg, uint8_t *value)
{
    int ret;
    uint8_t rx[5] = {0U, 0U, 0U, 0U, 0U};
    uint8_t frame_bytes = s_reg_frame_bytes;
    uint8_t reg_data_index;

    if (value == 0)
    {
        return -1;
    }

    ADS127L11_StopContinuousDma();
    ADS127L11_SPI4_ForceConfigMode();

    ret = ads_3wire_frame_reset_pattern();
    if (ret != 0)
    {
        return -10 + ret;
    }

    /* RREG 是 off-frame 命令，下一帧 NOP 才返回寄存器值。 */
    ret = ads_3wire_xfer_cmd_frame(ADS_CMD_RREG(reg), 0x00U, 0, frame_bytes);
    if (ret != 0)
    {
        return -20 + ret;
    }

    ret = ads_3wire_xfer_cmd_frame(ADS_CMD_NOP0, ADS_CMD_NOP0, rx, frame_bytes);
    if (ret != 0)
    {
        return -30 + ret;
    }

#if (APP_ADC_CONT_STATUS_ENABLE != 0U)
    reg_data_index = 1U;
#else
    reg_data_index = 0U;
#endif

    *value = rx[reg_data_index];
    return 0;
}

void ADS127L11_DumpRegisters(const char *tag)
{
    static const uint8_t regs[] = {
        ADS_REG_DEV_ID, 0x01U, ADS_REG_STATUS, ADS_REG_CONTROL,
        ADS_REG_MUX, ADS_REG_CONFIG1, ADS_REG_CONFIG2, ADS_REG_CONFIG3, ADS_REG_CONFIG4,
        0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU
    };
    static const char *names[] = {
        "DEV_ID", "REV_ID", "STATUS", "CONTROL",
        "MUX", "CONFIG1", "CONFIG2", "CONFIG3", "CONFIG4",
        "OFFSET2", "OFFSET1", "OFFSET0", "GAIN2", "GAIN1", "GAIN0", "CRC"
    };
    uint32_t i;

    myprintf("[ADS REG] dump begin: %s, frame_bytes=%u, status_en=%u\n",
             (tag != 0) ? tag : "",
             (unsigned)s_reg_frame_bytes,
             (unsigned)APP_ADC_CONT_STATUS_ENABLE);

    for (i = 0U; i < (sizeof(regs) / sizeof(regs[0])); i++)
    {
        uint8_t v = 0U;
        int ret = ADS127L11_ReadReg(regs[i], &v);
        if (ret == 0)
        {
            myprintf("[ADS REG] 0x%02X %-8s = 0x%02X\n", (unsigned)regs[i], names[i], (unsigned)v);
        }
        else
        {
            myprintf("[ADS REG] 0x%02X %-8s read FAIL ret=%d\n", (unsigned)regs[i], names[i], ret);
        }
    }

    myprintf("[ADS REG] dump end\n");
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
    ADS127L11_SPI4_ForceConfigMode();

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

    /*
     * CONFIG4:
     *   DATA=1 输出16bit，DATA=0 输出24bit；
     *   SPI_CRC=0，REG_CRC=0；
     *   STATUS 按 APP_ADC_CONT_STATUS_ENABLE 控制。
     *
     * 本版为连续 DMA 修正隐患，强制打开 STATUS 字节，所以 CONFIG4 bit0 = 1。
     */
    config4 = 0x00U;
    if (cfg->bits == APP_BITS_16)
    {
        config4 |= (uint8_t)(1U << 3);
    }
#if (APP_ADC_CONT_STATUS_ENABLE != 0U)
    config4 |= (uint8_t)(1U << 0);
#endif

    /* 写 CONFIG4 时仍使用“写入前”的输出帧长度；写完后再更新寄存器帧长度。 */
    if (ADS127L11_WriteReg(ADS_REG_CONFIG4, config4) != 0) return -6;

    s_reg_frame_bytes = ads_current_output_frame_bytes_from_cfg(cfg);

    /*
     * 关键自检：确认 CONFIG4 真正写入成功。
     * 如果 STATUS 位没有写进去，连续 DMA 会把每一个 SPI 轮询帧都当成样点，
     * 日志中的 +xxxxx/s 就会由 SPI 时钟决定，而不是由 ADC 采样率决定。
     */
    {
        uint8_t rb = 0U;
        uint8_t expected_mask = (uint8_t)(1U << 0); /* STATUS */
        uint8_t expected_value = (uint8_t)(1U << 0);

        expected_mask |= (uint8_t)(1U << 3);       /* DATA 16/24 */
        if (cfg->bits == APP_BITS_16)
        {
            expected_value |= (uint8_t)(1U << 3);
        }

        if (ADS127L11_ReadReg(ADS_REG_CONFIG4, &rb) != 0)
        {
            return -7;
        }

        if ((uint8_t)(rb & expected_mask) != expected_value)
        {
            myprintf("[ADS FAIL] CONFIG4 verify fail: read=0x%02X expect_mask=0x%02X expect=0x%02X\n",
                     (unsigned)rb,
                     (unsigned)expected_mask,
                     (unsigned)expected_value);
            return -8;
        }
    }

    return 0;
}

int ADS127L11_Init(const AppConfig_t *cfg)
{
    ADS127L11_BoardGpioInit();
    ADS127L11_SPI4_ForceConfigMode();

    /* 进入 3-wire 的关键：CS 低电平复位。 */
    ads_cs_low();
    ads_start_low();

    HAL_GPIO_WritePin(ADS127L11_AD_RESET_GPIO_Port, ADS127L11_AD_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(2);
    HAL_GPIO_WritePin(ADS127L11_AD_RESET_GPIO_Port, ADS127L11_AD_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(5);

    ads_clear_spi_rx_ovr();
    s_reg_frame_bytes = 3U; /* 复位后默认 24bit/no STATUS 输出帧 */

    if (ads_3wire_frame_reset_pattern() != 0)
    {
        return -10;
    }

    if (ADS127L11_Configure(cfg) != 0)
    {
        return -20;
    }

    /*
     * 按要求：初始化完成后等待 100ms，再读取全部关键寄存器。
     * 这里可以直接判断 CONFIG4 的 STATUS bit 是否已经被清掉。
     */
    HAL_Delay(100);
    ADS127L11_DumpRegisters("after init + 100ms");

    return 0;
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
    /* 3-wire 模式下 CS 必须一直保持低电平。 */
    ads_clear_spi_rx_ovr();
    ads_cs_low();
}

void ADS127L11_StreamExit(void)
{
    ADS127L11_StopContinuousDma();
    (void)ads_wait_spi_not_busy(ADS_SPI_BSY_TIMEOUT_LOOP);
    ads_cs_low();
    ads_clear_spi_rx_ovr();
}

int ADS127L11_PrepareContinuousDma(const AppConfig_t *cfg)
{
    uint8_t bps;
    uint8_t frame_bytes;

    if (!AppConfig_IsValid(cfg))
    {
        return -1;
    }

    ADS127L11_StopContinuousDma();

    bps = AppConfig_BytesPerSample(cfg);
#if (APP_ADC_CONT_STATUS_ENABLE != 0U)
    frame_bytes = (uint8_t)(bps + 1U); /* STATUS + data */
    s_cont_data_offset = 1U;
#else
    frame_bytes = bps;                /* data only, no STATUS */
    s_cont_data_offset = 0U;
#endif

    s_cont_bps = bps;
    s_cont_frame_bytes = frame_bytes;
    s_cont_half_bytes = (uint16_t)(APP_ADC_CONT_DMA_FRAMES_PER_HALF * frame_bytes);
    s_cont_total_bytes = (uint16_t)(APP_ADC_CONT_DMA_TOTAL_FRAMES * frame_bytes);

    if ((s_cont_total_bytes == 0U) || (s_cont_total_bytes > APP_ADC_CONT_DMA_MAX_BYTES))
    {
        s_dma_init_error++;
        return -2;
    }

    memset(s_dma_rx, 0, s_cont_total_bytes);
    s_dma_dummy = ADS_CMD_NOP0;

    ADS127L11_SPI4_ForceMode(ads_spi_prescaler_for_cfg(cfg));
    ads_clear_spi_rx_ovr();
    ads_cs_low();

    return 0;
}

static void ads_dma_disable_wait(DMA_Stream_TypeDef *s)
{
    uint32_t guard = 100000UL;

    s->CR &= ~DMA_SxCR_EN;
    while (((s->CR & DMA_SxCR_EN) != 0U) && (guard > 0U))
    {
        guard--;
    }
}

int ADS127L11_StartContinuousDmaFromDrdyIsr(void)
{
    uint32_t total = s_cont_total_bytes;

    if (s_cont_dma_running != 0U)
    {
        return 0;
    }

    if ((total == 0U) || (total > APP_ADC_CONT_DMA_MAX_BYTES))
    {
        s_dma_init_error++;
        return -1;
    }

    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_SPI4_CLK_ENABLE();

    ads_dma_disable_wait(DMA2_Stream0); /* SPI4_RX */
    ads_dma_disable_wait(DMA2_Stream1); /* SPI4_TX */

    DMA2->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0 |
                  DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1;

    ads_clear_spi_rx_ovr();
    ads_cs_low();

    s_dma_half_pending = 0U;
    s_dma_full_pending = 0U;

    SPI4->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);

    DMA2_Stream0->PAR  = (uint32_t)&SPI4->DR;
    DMA2_Stream0->M0AR = (uint32_t)s_dma_rx;
    DMA2_Stream0->NDTR = total;
    DMA2_Stream0->FCR  = 0U;
    DMA2_Stream0->CR   = SPI4_DMA_CHSEL |
                         DMA_SxCR_PL_1 |
                         DMA_SxCR_MINC |
                         DMA_SxCR_CIRC |
                         DMA_SxCR_HTIE |
                         DMA_SxCR_TCIE |
                         DMA_SxCR_TEIE |
                         DMA_SxCR_DMEIE;

    DMA2_Stream1->PAR  = (uint32_t)&SPI4->DR;
    DMA2_Stream1->M0AR = (uint32_t)&s_dma_dummy;
    DMA2_Stream1->NDTR = total;
    DMA2_Stream1->FCR  = 0U;
    DMA2_Stream1->CR   = SPI4_DMA_CHSEL |
                         DMA_SxCR_PL_1 |
                         DMA_SxCR_DIR_0 |
                         DMA_SxCR_CIRC |
                         DMA_SxCR_TEIE |
                         DMA_SxCR_DMEIE;

    DMA2_Stream0->CR |= DMA_SxCR_EN;
    SPI4->CR2 |= (SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    __HAL_SPI_ENABLE(&hspi4);
    DMA2_Stream1->CR |= DMA_SxCR_EN;

    s_dma_start_count++;
    s_cont_dma_running = 1U;
    return 0;
}

void ADS127L11_StopContinuousDma(void)
{
    s_cont_dma_running = 0U;
    s_dma_half_pending = 0U;
    s_dma_full_pending = 0U;

    SPI4->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    ads_dma_disable_wait(DMA2_Stream1);
    ads_dma_disable_wait(DMA2_Stream0);

    DMA2->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0 |
                  DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1;

    ads_cs_low();
    ads_clear_spi_rx_ovr();
}


void ADS127L11_PollContinuousDma(void)
{
    uint8_t do_half;
    uint8_t do_full;
    uint32_t primask;

    if (s_cont_dma_running == 0U)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    do_half = s_dma_half_pending;
    do_full = s_dma_full_pending;
    s_dma_half_pending = 0U;
    s_dma_full_pending = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }

    /* 注意：半缓冲长度加大到 4096 帧，400k/24bit + STATUS + 21MHz SPI 下
     * 每半缓冲约 6.2ms。主循环必须在同一半缓冲下一次被 DMA 覆盖前完成解析。
     */
    if (do_half != 0U)
    {
        AdcStream_OnContinuousDmaChunk(&s_dma_rx[0], s_cont_half_bytes);
    }

    if (do_full != 0U)
    {
        AdcStream_OnContinuousDmaChunk(&s_dma_rx[s_cont_half_bytes], s_cont_half_bytes);
    }
}

uint32_t ADS127L11_GetDmaPendingDropCount(void)
{
    return s_dma_pending_drop_count;
}

uint8_t ADS127L11_IsContinuousDmaRunning(void)
{
    return s_cont_dma_running;
}

uint8_t ADS127L11_GetContinuousFrameBytes(void)
{
    return s_cont_frame_bytes;
}

uint8_t ADS127L11_GetContinuousDataOffset(void)
{
    return s_cont_data_offset;
}

uint8_t ADS127L11_GetContinuousBytesPerSample(void)
{
    return s_cont_bps;
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

    ret = spi4_xfer_fast(ADS_CMD_NOP0, &dst[0]);
    if (ret != 0) { ads_clear_spi_rx_ovr(); return (int)(-10 + ret); }

    ret = spi4_xfer_fast(ADS_CMD_NOP0, &dst[1]);
    if (ret != 0) { ads_clear_spi_rx_ovr(); return (int)(-20 + ret); }

    if (bytes_per_sample == 3U)
    {
        ret = spi4_xfer_fast(ADS_CMD_NOP0, &dst[2]);
        if (ret != 0) { ads_clear_spi_rx_ovr(); return (int)(-30 + ret); }
    }

    return 0;
}

int ADS127L11_ReadDataDma(uint8_t *dst, uint8_t bytes_per_sample)
{
    return ADS127L11_ReadDataFast(dst, bytes_per_sample);
}

void ADS127L11_DmaCompleteFromIsr(void)
{
    s_dma_irq_count++;
}

void ADS127L11_DmaErrorFromIsr(void)
{
    s_dma_error_count++;
}

uint8_t ADS127L11_IsDmaBusy(void)
{
    return ADS127L11_IsContinuousDmaRunning();
}

uint32_t ADS127L11_GetDmaStartCount(void)
{
    return s_dma_start_count;
}

uint32_t ADS127L11_GetDmaCompleteCount(void)
{
    return s_dma_irq_count;
}

uint32_t ADS127L11_GetDmaErrorCount(void)
{
    return s_dma_error_count;
}

uint32_t ADS127L11_GetSpiTimeoutCount(void)
{
    return s_spi_timeout_count;
}

uint32_t ADS127L11_GetDmaInitError(void)
{
    return s_dma_init_error;
}

uint32_t ADS127L11_GetLastHalSpiError(void)
{
    return s_last_hal_spi_error;
}

uint32_t ADS127L11_GetLastHalSpiState(void)
{
    return s_last_hal_spi_state;
}

void DMA2_Stream0_IRQHandler(void)
{
    uint32_t lisr = DMA2->LISR;
    uint32_t feif = lisr & DMA_LISR_FEIF0;
    uint32_t fatal = lisr & (DMA_LISR_DMEIF0 | DMA_LISR_TEIF0);
    uint32_t ht  = lisr & DMA_LISR_HTIF0;
    uint32_t tc  = lisr & DMA_LISR_TCIF0;

    DMA2->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;

    /*
     * FEIF 在直通模式/高速连续传输中可能作为状态标志出现，
     * 不能直接把它当成致命错误停采。真正需要停采的是 TEIF/DMEIF。
     */
    if (feif != 0U)
    {
        s_dma_feif_count++;
    }

    if (fatal != 0U)
    {
        s_dma_error_count++;
        s_dma_fatal_count++;
        AdcStream_OnContinuousDmaError();
        return;
    }

    if (s_cont_dma_running == 0U)
    {
        return;
    }

    if (ht != 0U)
    {
        s_dma_irq_count++;
        if (s_dma_half_pending != 0U)
        {
            /* 主循环没有及时处理上一半缓冲：记录风险，但不中断采集。
             * 若该计数增加，说明已经存在丢样风险，需要继续加大 DMA 半缓冲或降低日志/上位机压力。
             */
            s_dma_pending_drop_count++;
        }
        s_dma_half_pending = 1U;
    }

    if (tc != 0U)
    {
        s_dma_irq_count++;
        if (s_dma_full_pending != 0U)
        {
            s_dma_pending_drop_count++;
        }
        s_dma_full_pending = 1U;
    }
}

void DMA2_Stream1_IRQHandler(void)
{
    uint32_t lisr = DMA2->LISR;
    uint32_t feif = lisr & DMA_LISR_FEIF1;
    uint32_t fatal = lisr & (DMA_LISR_DMEIF1 | DMA_LISR_TEIF1);

    DMA2->LIFCR = DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1;

    if (feif != 0U)
    {
        s_dma_feif_count++;
    }

    if (fatal != 0U)
    {
        s_dma_error_count++;
        s_dma_fatal_count++;
        AdcStream_OnContinuousDmaError();
    }
}

__weak void EXTI3_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(ADS127L11_DRDY_Pin);
}
