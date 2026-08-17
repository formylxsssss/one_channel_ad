#ifndef ADC_STREAM_H
#define ADC_STREAM_H

#include "app_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ADC_BLOCK_FREE   = 0,
    ADC_BLOCK_READY  = 1,
    ADC_BLOCK_QUEUED = 2
} AdcBlockState_t;

typedef struct
{
    volatile uint8_t state;
    uint8_t  bytes_per_sample;
    uint16_t sample_count;
    uint16_t frame_len;
    uint16_t data_bytes;
    uint32_t first_sample_seq;
    uint8_t  frame[APP_DATA_FRAME_MAX_LEN];
} AdcStreamBlock_t;

void AdcStream_Init(void);
void AdcStream_ResetDiscard(void);
int  AdcStream_Start(const AppConfig_t *cfg);
void AdcStream_Stop(void);
void AdcStream_Task(void);
void AdcStream_FlushPartialBlock(void);
uint8_t AdcStream_IsRunning(void);
uint32_t AdcStream_GetSampleSeq(void);
uint32_t AdcStream_GetOverrunCount(void);
uint32_t AdcStream_GetSpiErrorCount(void);
uint32_t AdcStream_GetFlags(void);
uint32_t AdcStream_GetDrdyIrqCount(void);
int32_t  AdcStream_GetLastSpiRet(void);
uint32_t AdcStream_GetIsrStopCount(void);
uint32_t AdcStream_GetLastStopFlags(void);
uint32_t AdcStream_GetDmaBusyCount(void);
uint32_t AdcStream_GetDmaErrorCount(void);
uint8_t  AdcStream_GetDrdyPinLevel(void);
uint16_t AdcStream_GetPartialSamples(void);
uint16_t AdcStream_GetPartialBytes(void);
uint16_t AdcStream_GetReadyCount(void);
uint16_t AdcStream_GetQueuedCount(void);
uint16_t AdcStream_GetFreeCount(void);

/* 额外调试统计：需要时可在 RTT 里临时打印。 */
uint32_t AdcStream_GetContDmaChunkCount(void);
uint32_t AdcStream_GetStatusNewCount(void);
uint32_t AdcStream_GetStatusSkipCount(void);
uint32_t AdcStream_GetFrameParseCount(void);

AdcStreamBlock_t *AdcStream_PeekReadyBlock(void);
void AdcStream_MarkBlockQueued(AdcStreamBlock_t *blk);
AdcStreamBlock_t *AdcStream_PeekQueuedBlock(void);
void AdcStream_ReleaseQueuedBlock(void);

/* DRDY 下降沿中断入口：本方案只用第一个 DRDY 做 DMA 同步启动。 */
void AdcStream_OnDrdyIrq(void);

/* SPI4 RX DMA 半满/全满批量入口。 */
void AdcStream_OnContinuousDmaChunk(const uint8_t *buf, uint16_t len);
void AdcStream_OnContinuousDmaError(void);

/* 兼容旧版本 DMA 回调入口。 */
void AdcStream_OnSpiDmaDone(void);
void AdcStream_OnSpiDmaError(void);

#ifdef __cplusplus
}
#endif

#endif
