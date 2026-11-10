#include "app_config.h"

AppConfig_t g_app_cfg = { APP_DEFAULT_FS_HZ, APP_DEFAULT_BITS, APP_DEFAULT_RANGE, 0U, 0U };

AppConfig_t AppConfig_Default(void)
{
    AppConfig_t cfg;

    cfg.fs_hz = APP_DEFAULT_FS_HZ;
    cfg.bits = APP_DEFAULT_BITS;
    cfg.range = APP_DEFAULT_RANGE;
    cfg.reserved0 = 0U;
    cfg.reserved1 = 0U;

    return cfg;
}

int AppConfig_IsValid(const AppConfig_t *cfg)
{
    if (cfg == 0)
    {
        return 0;
    }

    if (!((cfg->fs_hz == APP_FS_400K) ||
          (cfg->fs_hz == APP_FS_200K) ||
          (cfg->fs_hz == APP_FS_100K) ||
          (cfg->fs_hz == APP_FS_50K)  ||
          (cfg->fs_hz == APP_FS_25K)))
    {
        return 0;
    }

    if (!((cfg->bits == APP_BITS_24) || (cfg->bits == APP_BITS_16)))
    {
        return 0;
    }

    if (!((cfg->range == APP_GAIN_0DB) ||
          (cfg->range == APP_GAIN_20DB) ||
          (cfg->range == APP_GAIN_40DB)))
    {
        return 0;
    }

    return 1;
}

uint8_t AppConfig_BytesPerSample(const AppConfig_t *cfg)
{
    if ((cfg != 0) && (cfg->bits == APP_BITS_16))
    {
        return 2U;
    }

    return 3U;
}

/* Round the physical conversion rate for the integer-valued DATA protocol. */
uint32_t AppConfig_ActualFsHz(const AppConfig_t *cfg)
{
    if (!AppConfig_IsValid(cfg))
    {
        return 0U;
    }
    return (uint32_t)(((uint64_t)cfg->fs_hz * APP_ADC_CLOCK_HZ +
                       APP_ADC_NOMINAL_CLOCK_HZ / 2U) / APP_ADC_NOMINAL_CLOCK_HZ);
}

/* Retain the nominal selectors' original wideband OSR 32/64/128/256/512. */
uint8_t AppConfig_AdcFilterCode(uint32_t fs_hz)
{
    switch (fs_hz)
    {
    case APP_FS_400K:
        return 0x00U;

    case APP_FS_200K:
        return 0x01U;

    case APP_FS_100K:
        return 0x02U;

    case APP_FS_50K:
        return 0x03U;

    case APP_FS_25K:
        return 0x04U;

    default:
        return 0x00U;
    }
}

uint8_t AppConfig_RangeToGainDb(uint8_t range)
{
    switch (range)
    {
    case APP_GAIN_0DB:
        return 0U;

    case APP_GAIN_20DB:
        return 20U;

    case APP_GAIN_40DB:
        return 40U;

    default:
        return 0xFFU;
    }
}

uint16_t AppConfig_RangeToGainX(uint8_t range)
{
    switch (range)
    {
    case APP_GAIN_0DB:
        return 1U;

    case APP_GAIN_20DB:
        return 10U;

    case APP_GAIN_40DB:
        return 100U;

    default:
        return 0U;
    }
}

const char *AppConfig_RangeName(uint8_t range)
{
    switch (range)
    {
    case APP_GAIN_0DB:
        return "0dB";

    case APP_GAIN_20DB:
        return "20dB";

    case APP_GAIN_40DB:
        return "40dB";

    default:
        return "INVALID";
    }
}
