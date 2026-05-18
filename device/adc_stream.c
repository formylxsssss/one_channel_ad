#include "adc_stream.h"
#include "ads127l11.h"
#include "main.h"
#include <string.h>

static AdcStreamBlock_t s_blocks[APP_ADC_BLOCK_COUNT];

static volatile uint8_t  s_running = 0;
static volatile uint8_t  s_bps = 3;
static volatile uint8_t  s_wr = 0;
static volatile uint8_t  s_ready_idx = 0;
static volatile uint8_t  s_queued_idx = 0;
static volatile uint16_t s_wr_samples = 0;
static volatile uint16_t s_wr_bytes = 0;

static volatile uint32_t s_sample_seq = 0;
static volatile uint32_t s_overrun = 0;
static volatile uint32_t s_spi_error = 0;
static volatile uint32_t s_flags = 0;
static volatile uint32_t s_drdy_irq_count = 0;
static volatile int32_t  s_last_spi_ret = 0;
static volatile uint32_t s_isr_stop_count = 0;
static volatile uint32_t s_last_stop_flags = 0;

/* 兼容旧日志字段：这里不再表示 DMA，而表示寄存器快速读取异常统计。 */
static volatile uint32_t s_fast_read_fail_count = 0;
static volatile uint32_t s_fast_read_busy_count = 0;

static inline uint8_t next_idx(uint8_t i)
{
    i++;
    if (i >= APP_ADC_BLOCK_COUNT)
    {
        i = 0;
    }
    return i;
}

static void clear_state_common(void)
{
    memset(s_blocks, 0, sizeof(s_blocks));

    s_bps = 3;
    s_wr = 0;
    s_ready_idx = 0;
    s_queued_idx = 0;
    s_wr_samples = 0;
    s_wr_bytes = 0;

    s_sample_seq = 0;
    s_overrun = 0;
    s_spi_error = 0;
    s_flags = 0;
    s_drdy_irq_count = 0;
    s_last_spi_ret = 0;
    s_isr_stop_count = 0;
    s_last_stop_flags = 0;
    s_fast_read_fail_count = 0;
    s_fast_read_busy_count = 0;
}

void AdcStream_Init(void)
{
    s_running = 0;
    clear_state_common();
}

void AdcStream_ResetDiscard(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    s_running = 0;
    clear_state_common();

    if (primask == 0U)
    {
        __enable_irq();
    }
}

int AdcStream_Start(const AppConfig_t *cfg)
{
    uint32_t primask;

    if (!AppConfig_IsValid(cfg))
    {
        return -1;
    }

    ADS127L11_Stop();
    ADS127L11_StreamExit();

    primask = __get_PRIMASK();
    __disable_irq();
    clear_state_common();
    s_bps = AppConfig_BytesPerSample(cfg);
    s_running = 1U;
    if (primask == 0U)
    {
        __enable_irq();
    }

    /* 进入采样流模式后保持 CS 为低；DRDY 中断里只读 2/3 字节数据。 */
    ADS127L11_StreamEnter();
    ADS127L11_Start();

    return 0;
}

void AdcStream_FlushPartialBlock(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if ((s_wr_samples > 0U) && (s_blocks[s_wr].state == ADC_BLOCK_FREE))
    {
        AdcStreamBlock_t *blk = &s_blocks[s_wr];
        blk->sample_count = s_wr_samples;
        blk->data_bytes = s_wr_bytes;
        blk->bytes_per_sample = s_bps;
        blk->frame_len = (uint16_t)(APP_FRAME_HDR_LEN + APP_DATA_META_LEN + s_wr_bytes + APP_FRAME_CRC_LEN);
        blk->state = ADC_BLOCK_READY;
        s_wr = next_idx(s_wr);
        s_wr_samples = 0;
        s_wr_bytes = 0;
    }

    if (primask == 0U)
    {
        __enable_irq();
    }
}

void AdcStream_Stop(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_running = 0;
    if (primask == 0U)
    {
        __enable_irq();
    }

    ADS127L11_Stop();
    ADS127L11_StreamExit();
    AdcStream_FlushPartialBlock();
}

void AdcStream_Task(void)
{
    /* DRDY + SPI4 寄存器快速读取版不需要后台处理 DMA 状态。 */
}

uint8_t AdcStream_IsRunning(void)
{
    return s_running;
}

uint32_t AdcStream_GetSampleSeq(void)
{
    return s_sample_seq;
}

uint32_t AdcStream_GetOverrunCount(void)
{
    return s_overrun;
}

uint32_t AdcStream_GetSpiErrorCount(void)
{
    return s_spi_error;
}

uint32_t AdcStream_GetFlags(void)
{
    return s_flags;
}

uint32_t AdcStream_GetDrdyIrqCount(void)
{
    return s_drdy_irq_count;
}

int32_t AdcStream_GetLastSpiRet(void)
{
    return s_last_spi_ret;
}

uint32_t AdcStream_GetIsrStopCount(void)
{
    return s_isr_stop_count;
}

uint32_t AdcStream_GetLastStopFlags(void)
{
    return s_last_stop_flags;
}

uint32_t AdcStream_GetDmaBusyCount(void)
{
    return s_fast_read_busy_count;
}

uint32_t AdcStream_GetDmaErrorCount(void)
{
    return s_fast_read_fail_count;
}

uint8_t AdcStream_GetDrdyPinLevel(void)
{
#ifdef DRDY_Pin
    return (uint8_t)HAL_GPIO_ReadPin(DRDY_GPIO_Port, DRDY_Pin);
#else
    return (uint8_t)HAL_GPIO_ReadPin(ADS127L11_DRDY_GPIO_Port, ADS127L11_DRDY_Pin);
#endif
}

uint16_t AdcStream_GetPartialSamples(void)
{
    return s_wr_samples;
}

uint16_t AdcStream_GetPartialBytes(void)
{
    return s_wr_bytes;
}

uint16_t AdcStream_GetReadyCount(void)
{
    uint16_t n = 0;

    for (uint16_t i = 0; i < APP_ADC_BLOCK_COUNT; i++)
    {
        if (s_blocks[i].state == ADC_BLOCK_READY)
        {
            n++;
        }
    }

    return n;
}

uint16_t AdcStream_GetQueuedCount(void)
{
    uint16_t n = 0;

    for (uint16_t i = 0; i < APP_ADC_BLOCK_COUNT; i++)
    {
        if (s_blocks[i].state == ADC_BLOCK_QUEUED)
        {
            n++;
        }
    }

    return n;
}

uint16_t AdcStream_GetFreeCount(void)
{
    uint16_t n = 0;

    for (uint16_t i = 0; i < APP_ADC_BLOCK_COUNT; i++)
    {
        if (s_blocks[i].state == ADC_BLOCK_FREE)
        {
            n++;
        }
    }

    return n;
}

AdcStreamBlock_t *AdcStream_PeekReadyBlock(void)
{
    if (s_blocks[s_ready_idx].state == ADC_BLOCK_READY)
    {
        return &s_blocks[s_ready_idx];
    }

    return 0;
}

void AdcStream_MarkBlockQueued(AdcStreamBlock_t *blk)
{
    if (blk == 0)
    {
        return;
    }

    if (blk->state == ADC_BLOCK_READY)
    {
        blk->state = ADC_BLOCK_QUEUED;
        s_ready_idx = next_idx(s_ready_idx);
    }
}

AdcStreamBlock_t *AdcStream_PeekQueuedBlock(void)
{
    if (s_blocks[s_queued_idx].state == ADC_BLOCK_QUEUED)
    {
        return &s_blocks[s_queued_idx];
    }

    return 0;
}

void AdcStream_ReleaseQueuedBlock(void)
{
    if (s_blocks[s_queued_idx].state == ADC_BLOCK_QUEUED)
    {
        s_blocks[s_queued_idx].state = ADC_BLOCK_FREE;
        s_queued_idx = next_idx(s_queued_idx);
    }
}

static inline void adc_stream_store_one_sample_from_drdy(void)
{
    AdcStreamBlock_t *blk;
    uint16_t pos;
    uint8_t bps;
    int ret;

    bps = s_bps;
    blk = &s_blocks[s_wr];

    /* TCP 发送不够快导致没有空块：不停止采样，记录 overrun，跳过当前样点。 */
    if (blk->state != ADC_BLOCK_FREE)
    {
        s_overrun++;
        s_sample_seq++;
        s_flags |= (APP_STREAM_FLAG_OVERRUN_STOPPED | APP_STREAM_FLAG_TCP_BACKPRESSURE);
        return;
    }

    if (s_wr_samples == 0U)
    {
        blk->first_sample_seq = s_sample_seq;
        blk->bytes_per_sample = bps;
        blk->sample_count = 0;
        blk->data_bytes = 0;
        blk->frame_len = 0;
    }

    pos = (uint16_t)(APP_FRAME_HDR_LEN + APP_DATA_META_LEN + s_wr_bytes);

    if (((uint32_t)pos + bps) > sizeof(blk->frame))
    {
        s_overrun++;
        s_sample_seq++;
        s_flags |= APP_STREAM_FLAG_OVERRUN_STOPPED;
        return;
    }

    ret = ADS127L11_ReadDataFast(&blk->frame[pos], bps);
    s_last_spi_ret = ret;

    if (ret != 0)
    {
        s_spi_error++;
        s_fast_read_fail_count++;
        s_overrun++;
        s_sample_seq++;
        s_flags |= APP_STREAM_FLAG_SPI_TIMEOUT;

        if ((ret == -13) || (ret == -14) || (ret == -23) || (ret == -24) ||
            (ret == -33) || (ret == -34))
        {
            s_fast_read_busy_count++;
        }
        return;
    }

    s_wr_bytes = (uint16_t)(s_wr_bytes + bps);
    s_wr_samples++;
    s_sample_seq++;

    if (s_wr_samples >= APP_ADC_BLOCK_SAMPLES)
    {
        blk->sample_count = s_wr_samples;
        blk->data_bytes = s_wr_bytes;
        blk->bytes_per_sample = bps;
        blk->frame_len = (uint16_t)(APP_FRAME_HDR_LEN + APP_DATA_META_LEN + s_wr_bytes + APP_FRAME_CRC_LEN);
        blk->state = ADC_BLOCK_READY;

        s_wr = next_idx(s_wr);
        s_wr_samples = 0;
        s_wr_bytes = 0;
    }
}

void AdcStream_OnDrdyIrq(void)
{
    if (s_running == 0U)
    {
        return;
    }

    s_drdy_irq_count++;
    adc_stream_store_one_sample_from_drdy();
}

void AdcStream_OnSpiDmaDone(void)
{
    /* 本版本不使用 SPI DMA。 */
}

void AdcStream_OnSpiDmaError(void)
{
    s_spi_error++;
    s_fast_read_fail_count++;
    s_flags |= APP_STREAM_FLAG_SPI_DMA_ERROR;
}

void AdcStream_OnContinuousDmaChunk(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}

void AdcStream_OnContinuousDmaError(void)
{
    s_spi_error++;
    s_fast_read_fail_count++;
    s_flags |= APP_STREAM_FLAG_CONT_DMA_ERROR;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
#ifdef DRDY_Pin
    if (GPIO_Pin == DRDY_Pin)
    {
        AdcStream_OnDrdyIrq();
    }
#else
    if (GPIO_Pin == ADS127L11_DRDY_Pin)
    {
        AdcStream_OnDrdyIrq();
    }
#endif
}

/*
 * 当前原理图 DRDY 固定 PE3。
 * 如果 stm32f4xx_it.c 已经有 EXTI3_IRQHandler，那里通常会调用 HAL_GPIO_EXTI_IRQHandler，
 * 本 weak 函数不会参与链接；如果没有，则使用这里的实现。
 */
__weak void EXTI3_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(ADS127L11_DRDY_Pin);
}
