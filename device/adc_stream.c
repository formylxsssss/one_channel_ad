#include "adc_stream.h"
#include "ads127l11.h"
#include "main.h"
#include <string.h>

static AdcStreamBlock_t s_blocks[APP_ADC_BLOCK_COUNT];

static volatile uint8_t  s_running = 0;
static volatile uint8_t  s_wait_first_drdy = 0;
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
static volatile uint8_t  s_stop_request = 0;

/* 本连续 DMA 版本中：
 *   dma_busy_count  = STATUS 非新数据/跳过帧计数
 *   dma_error_count = DMA 错误/帧错误计数
 */
static volatile uint32_t s_dma_busy_count = 0;
static volatile uint32_t s_dma_error_count = 0;
static volatile uint32_t s_cont_dma_chunk_count = 0;
static volatile uint32_t s_status_new_count = 0;
static volatile uint32_t s_status_skip_count = 0;
static volatile uint32_t s_frame_parse_count = 0;

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

    s_wait_first_drdy = 0;
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
    s_stop_request = 0;

    s_dma_busy_count = 0;
    s_dma_error_count = 0;
    s_cont_dma_chunk_count = 0;
    s_status_new_count = 0;
    s_status_skip_count = 0;
    s_frame_parse_count = 0;
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
    s_wait_first_drdy = 0;
    clear_state_common();

    if (primask == 0U)
    {
        __enable_irq();
    }

    ADS127L11_StopContinuousDma();
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

    if (ADS127L11_PrepareContinuousDma(cfg) != 0)
    {
        return -2;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    clear_state_common();
    s_bps = AppConfig_BytesPerSample(cfg);
    s_running = 1U;
    s_wait_first_drdy = 1U;
    s_flags |= APP_STREAM_FLAG_WAIT_FIRST_DRDY;
    if (primask == 0U)
    {
        __enable_irq();
    }

    __HAL_GPIO_EXTI_CLEAR_IT(ADS127L11_DRDY_Pin);
    HAL_NVIC_EnableIRQ(EXTI3_IRQn);

    /* 先 CS 拉低，然后 START 拉高；第一个 DRDY 下降沿里启动连续 SPI DMA。 */
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
    s_wait_first_drdy = 0;
    s_stop_request = 0;
    if (primask == 0U)
    {
        __enable_irq();
    }

    HAL_NVIC_EnableIRQ(EXTI3_IRQn);
    ADS127L11_Stop();
    ADS127L11_StreamExit();
    AdcStream_FlushPartialBlock();
}

void AdcStream_Task(void)
{
    /*
     * 性能版关键点：连续 SPI DMA 的半缓冲解析放在主循环执行，
     * DMA IRQ 只置 pending 标志，避免长时间占用中断导致以太网/TCP 来不及处理。
     */
    ADS127L11_PollContinuousDma();

    if (s_stop_request != 0U)
    {
        s_stop_request = 0;
        ADS127L11_Stop();
        ADS127L11_StreamExit();
        AdcStream_FlushPartialBlock();
    }
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
    return s_dma_busy_count;
}

uint32_t AdcStream_GetDmaErrorCount(void)
{
    return s_dma_error_count;
}

uint8_t AdcStream_GetDrdyPinLevel(void)
{
    return (uint8_t)HAL_GPIO_ReadPin(ADS127L11_DRDY_GPIO_Port, ADS127L11_DRDY_Pin);
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

uint32_t AdcStream_GetContDmaChunkCount(void)
{
    return s_cont_dma_chunk_count;
}

uint32_t AdcStream_GetStatusNewCount(void)
{
    return s_status_new_count;
}

uint32_t AdcStream_GetStatusSkipCount(void)
{
    return s_status_skip_count;
}

uint32_t AdcStream_GetFrameParseCount(void)
{
    return s_frame_parse_count;
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

static inline void adc_stream_finish_block_if_full(AdcStreamBlock_t *blk)
{
    if (s_wr_samples >= APP_ADC_BLOCK_SAMPLES)
    {
        blk->sample_count = s_wr_samples;
        blk->data_bytes = s_wr_bytes;
        blk->bytes_per_sample = s_bps;
        blk->frame_len = (uint16_t)(APP_FRAME_HDR_LEN + APP_DATA_META_LEN + s_wr_bytes + APP_FRAME_CRC_LEN);
        blk->state = ADC_BLOCK_READY;

        s_wr = next_idx(s_wr);
        s_wr_samples = 0;
        s_wr_bytes = 0;
    }
}

static inline void adc_stream_store_sample_bytes(const uint8_t *src, uint8_t bps)
{
    AdcStreamBlock_t *blk;
    uint16_t pos;

    blk = &s_blocks[s_wr];

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

    if (bps == 2U)
    {
        blk->frame[pos + 0U] = src[0];
        blk->frame[pos + 1U] = src[1];
    }
    else
    {
        blk->frame[pos + 0U] = src[0];
        blk->frame[pos + 1U] = src[1];
        blk->frame[pos + 2U] = src[2];
    }

    s_wr_bytes = (uint16_t)(s_wr_bytes + bps);
    s_wr_samples++;
    s_sample_seq++;

    adc_stream_finish_block_if_full(blk);
}

void AdcStream_OnDrdyIrq(void)
{
    if (s_running == 0U)
    {
        return;
    }

    if (s_wait_first_drdy != 0U)
    {
        s_wait_first_drdy = 0U;
        s_flags &= ~APP_STREAM_FLAG_WAIT_FIRST_DRDY;

        if (ADS127L11_StartContinuousDmaFromDrdyIsr() == 0)
        {
            s_flags |= APP_STREAM_FLAG_DMA_STARTED;
            /* DMA 已经和第一个 DRDY 同步，后续不再需要每个 DRDY 都进中断。 */
            HAL_NVIC_DisableIRQ(EXTI3_IRQn);
            __HAL_GPIO_EXTI_CLEAR_IT(ADS127L11_DRDY_Pin);
        }
        else
        {
            s_spi_error++;
            s_dma_error_count++;
            s_last_stop_flags = s_flags | APP_STREAM_FLAG_SPI_DMA_ERROR;
            s_flags = s_last_stop_flags;
            s_running = 0U;
            s_stop_request = 1U;
            s_isr_stop_count++;
        }
    }
}

void AdcStream_OnContinuousDmaChunk(const uint8_t *buf, uint16_t len)
{
    uint8_t frame_bytes;
    uint8_t bps;
    uint8_t data_offset;
    uint16_t i;

    if ((s_running == 0U) || (buf == 0) || (len == 0U))
    {
        return;
    }

    frame_bytes = ADS127L11_GetContinuousFrameBytes();
    data_offset = ADS127L11_GetContinuousDataOffset();
    bps = ADS127L11_GetContinuousBytesPerSample();

    if ((frame_bytes == 0U) || (bps == 0U) || ((uint16_t)frame_bytes > len))
    {
        s_dma_error_count++;
        s_flags |= APP_STREAM_FLAG_FRAME_ALIGN_WARN;
        return;
    }

    s_cont_dma_chunk_count++;

    for (i = 0U; (uint16_t)(i + frame_bytes) <= len; i = (uint16_t)(i + frame_bytes))
    {
        const uint8_t *fr = &buf[i];
        s_frame_parse_count++;

#if (APP_ADC_CONT_STATUS_ENABLE != 0U)
#if (APP_ADC_CONT_REQUIRE_STATUS_DRDY != 0U)
        if ((fr[0] & APP_ADS_STATUS_DRDY_BIT) == 0U)
        {
            /* 这是连续 SPI 轮询读到的旧数据帧，不是新 ADC 样点。 */
            s_status_skip_count++;
            s_dma_busy_count++;
            continue;
        }
#endif
        s_status_new_count++;
        adc_stream_store_sample_bytes(&fr[data_offset], bps);
#else
        /* 无 STATUS 时无法区分旧帧/新帧，只能作为低速临时调试模式。 */
        s_flags |= APP_STREAM_FLAG_FRAME_ALIGN_WARN;
        adc_stream_store_sample_bytes(fr, bps);
#endif
    }

    if ((s_frame_parse_count > 8192UL) && (s_status_new_count == 0UL))
    {
        /* DMA 在跑，但一直解析不到 STATUS.DRDY=1：大概率是 SPI 帧没有和 DRDY 对齐。 */
        s_flags |= APP_STREAM_FLAG_STATUS_NO_NEW;
    }
}

void AdcStream_OnContinuousDmaError(void)
{
    s_spi_error++;
    s_dma_error_count++;
    s_last_stop_flags = s_flags | APP_STREAM_FLAG_CONT_DMA_ERROR | APP_STREAM_FLAG_SPI_DMA_ERROR | APP_STREAM_FLAG_DMA_PENDING_DROP;
    s_flags = s_last_stop_flags;
    s_running = 0U;
    s_stop_request = 1U;
    s_isr_stop_count++;
}

void AdcStream_OnSpiDmaDone(void)
{
    /* 本版本不使用每样点 HAL SPI DMA。 */
}

void AdcStream_OnSpiDmaError(void)
{
    AdcStream_OnContinuousDmaError();
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ADS127L11_DRDY_Pin)
    {
        AdcStream_OnDrdyIrq();
    }
}
