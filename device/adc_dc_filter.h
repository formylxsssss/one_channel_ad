#ifndef ADC_DC_FILTER_H
#define ADC_DC_FILTER_H

#include <stdint.h>

typedef struct
{
    int64_t mean_q16;
    uint32_t alpha_q20;
    uint8_t seeded;
} AdcDcFilter_t;

static inline void AdcDcFilter_Init(AdcDcFilter_t *filter, uint32_t fs_hz, uint32_t cutoff_hz)
{
    uint64_t omega = 6283185ULL * cutoff_hz;
    uint64_t denominator = (uint64_t)fs_hz * 1000000ULL + omega;

    filter->mean_q16 = 0;
    filter->seeded = 0U;
    /* alpha = 2*pi*fc / (fs + 2*pi*fc), calculated only at START. */
    filter->alpha_q20 = (uint32_t)((omega * 1048576ULL + denominator / 2U) / denominator);
}

static inline int32_t AdcDcFilter_Apply(AdcDcFilter_t *filter, int32_t sample)
{
    int64_t sample_q16 = (int64_t)sample * 65536LL;
    int64_t delta;

    if (filter->seeded == 0U)
    {
        filter->mean_q16 = sample_q16;
        filter->seeded = 1U;
        return 0;
    }

    delta = sample_q16 - filter->mean_q16;
    /* Q16 state and Q20 coefficient keep full-scale 24-bit products in int64. */
    filter->mean_q16 += (delta * filter->alpha_q20) >> 20;
    return (int32_t)((sample_q16 - filter->mean_q16) / 65536LL);
}

#endif
