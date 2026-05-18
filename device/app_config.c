#include "app_config.h"

AppConfig_t g_app_cfg = { APP_DEFAULT_FS_HZ, APP_DEFAULT_BITS, APP_DEFAULT_RANGE, 0, 0 };

AppConfig_t AppConfig_Default(void)
{
    AppConfig_t cfg;
    cfg.fs_hz = APP_DEFAULT_FS_HZ;
    cfg.bits = APP_DEFAULT_BITS;
    cfg.range = APP_DEFAULT_RANGE;
    cfg.reserved0 = 0;
    cfg.reserved1 = 0;
    return cfg;
}

int AppConfig_IsValid(const AppConfig_t *cfg)
{
    if (cfg == 0) return 0;

    if (!((cfg->fs_hz == APP_FS_400K) || (cfg->fs_hz == APP_FS_200K) ||
          (cfg->fs_hz == APP_FS_100K) || (cfg->fs_hz == APP_FS_50K)  ||
          (cfg->fs_hz == APP_FS_25K))) return 0;

    if (!((cfg->bits == APP_BITS_24) || (cfg->bits == APP_BITS_16))) return 0;
    if (cfg->range > APP_RANGE_D3_1M) return 0;

    return 1;
}

uint8_t AppConfig_BytesPerSample(const AppConfig_t *cfg)
{
    return (cfg->bits == APP_BITS_16) ? 2U : 3U;
}

/* ADS127L11 CONFIG3 FILTER[4:0]：wideband OSR 32/64/128/256/512。
 * 在 25.6MHz high-speed 内部时钟下对应约 400k/200k/100k/50k/25kSPS。
 */
uint8_t AppConfig_AdcFilterCode(uint32_t fs_hz)
{
    switch (fs_hz)
    {
    case APP_FS_400K: return 0x00U; /* wideband OSR=32  */
    case APP_FS_200K: return 0x01U; /* wideband OSR=64  */
    case APP_FS_100K: return 0x02U; /* wideband OSR=128 */
    case APP_FS_50K:  return 0x03U; /* wideband OSR=256 */
    case APP_FS_25K:  return 0x04U; /* wideband OSR=512 */
    default:          return 0x00U;
    }
}
