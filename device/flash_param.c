#include "main.h"
#include "flash_param.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_flash_ex.h"
#include <string.h>
#include <stddef.h>

/*
 * STM32F427VGT6 1MB Flash：默认使用最后一个 128KB sector。
 * app_config.h 中可通过 APP_PARAM_FLASH_ADDR / APP_PARAM_FLASH_SECTOR 覆盖。
 */

typedef struct
{
    uint32_t magic;
    uint32_t version;

    uint32_t fs_hz;
    uint8_t  bits;
    uint8_t  range;
    uint8_t  app_reserved0;
    uint8_t  app_reserved1;

    uint8_t  local_ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    uint8_t  server_ip[4];

    uint16_t server_port;
    uint8_t  device_id;
    uint8_t  net_reserved0;

    uint32_t reconnect_ms;
    uint32_t net_reserved1;

    uint16_t crc16;
    uint16_t pad;
} FlashParamRecord_t;

static uint16_t crc16_modbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0U; j < 8U; j++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

static void record_from_configs(FlashParamRecord_t *rec,
                                const AppConfig_t *cfg,
                                const AppNetConfig_t *net)
{
    memset(rec, 0, sizeof(*rec));

    rec->magic = APP_PARAM_MAGIC;
    rec->version = APP_PARAM_VERSION;

    rec->fs_hz = cfg->fs_hz;
    rec->bits = cfg->bits;
    rec->range = cfg->range;
    rec->app_reserved0 = 0U;
    rec->app_reserved1 = 0U;

    memcpy(rec->local_ip, net->local_ip, 4U);
    memcpy(rec->netmask, net->netmask, 4U);
    memcpy(rec->gateway, net->gateway, 4U);
    memcpy(rec->server_ip, net->server_ip, 4U);

    rec->server_port = net->server_port;
    rec->device_id = net->device_id;
    rec->net_reserved0 = 0U;
    rec->reconnect_ms = net->reconnect_ms;
    rec->net_reserved1 = 0U;

    rec->crc16 = crc16_modbus((const uint8_t *)rec, offsetof(FlashParamRecord_t, crc16));
    rec->pad = 0U;
}

static void configs_from_record(const FlashParamRecord_t *rec,
                                AppConfig_t *cfg,
                                AppNetConfig_t *net)
{
    if (cfg != 0)
    {
        cfg->fs_hz = rec->fs_hz;
        cfg->bits = rec->bits;
        cfg->range = rec->range;
        cfg->reserved0 = 0U;
        cfg->reserved1 = 0U;
    }

    if (net != 0)
    {
        memcpy(net->local_ip, rec->local_ip, 4U);
        memcpy(net->netmask, rec->netmask, 4U);
        memcpy(net->gateway, rec->gateway, 4U);
        memcpy(net->server_ip, rec->server_ip, 4U);

        net->server_port = rec->server_port;
        net->device_id = rec->device_id;
        net->reserved0 = 0U;
        net->reconnect_ms = rec->reconnect_ms;
        net->reserved1 = 0U;
    }
}

static int flash_read_record(FlashParamRecord_t *out)
{
    const FlashParamRecord_t *rec = (const FlashParamRecord_t *)APP_PARAM_FLASH_ADDR;
    uint16_t calc;

    if (out == 0)
    {
        return -1;
    }

    memcpy(out, rec, sizeof(*out));

    if (out->magic != APP_PARAM_MAGIC)
    {
        return -2;
    }

    if (out->version != APP_PARAM_VERSION)
    {
        return -3;
    }

    calc = crc16_modbus((const uint8_t *)out, offsetof(FlashParamRecord_t, crc16));
    if (out->crc16 != calc)
    {
        return -4;
    }

    return 0;
}

static int flash_write_record(const FlashParamRecord_t *rec)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0U;
    const uint8_t *p;

    if (rec == 0)
    {
        return -1;
    }

    HAL_FLASH_Unlock();

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = APP_PARAM_FLASH_SECTOR;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -2;
    }

    p = (const uint8_t *)rec;

    for (uint32_t i = 0U; i < sizeof(*rec); i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, APP_PARAM_FLASH_ADDR + i, p[i]) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return -3;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

int FlashParam_LoadAll(AppConfig_t *cfg, AppNetConfig_t *net)
{
    FlashParamRecord_t rec;
    int ret;

    if ((cfg == 0) || (net == 0))
    {
        return -1;
    }

    ret = flash_read_record(&rec);
    if (ret != 0)
    {
        *cfg = AppConfig_Default();
        *net = AppNetConfig_Default();
        return ret;
    }

    configs_from_record(&rec, cfg, net);

    if (!AppConfig_IsValid(cfg))
    {
        *cfg = AppConfig_Default();
        *net = AppNetConfig_Default();
        return -10;
    }

    if (!AppNetConfig_IsValid(net))
    {
        *cfg = AppConfig_Default();
        *net = AppNetConfig_Default();
        return -11;
    }

    return 0;
}

int FlashParam_SaveAll(const AppConfig_t *cfg, const AppNetConfig_t *net)
{
    FlashParamRecord_t rec;

    if ((cfg == 0) || (net == 0))
    {
        return -1;
    }

    if (!AppConfig_IsValid(cfg))
    {
        return -2;
    }

    if (!AppNetConfig_IsValid(net))
    {
        return -3;
    }

    record_from_configs(&rec, cfg, net);
    return flash_write_record(&rec);
}

int FlashParam_Load(AppConfig_t *cfg)
{
    AppNetConfig_t net;

    if (cfg == 0)
    {
        return -1;
    }

    return FlashParam_LoadAll(cfg, &net);
}

int FlashParam_Save(const AppConfig_t *cfg)
{
    AppConfig_t old_cfg;
    AppNetConfig_t net;
    int ret;

    if ((cfg == 0) || (!AppConfig_IsValid(cfg)))
    {
        return -1;
    }

    ret = FlashParam_LoadAll(&old_cfg, &net);
    if (ret != 0)
    {
        net = AppNetConfig_Default();
    }

    return FlashParam_SaveAll(cfg, &net);
}

int FlashParam_LoadNet(AppNetConfig_t *cfg)
{
    AppConfig_t app;

    if (cfg == 0)
    {
        return -1;
    }

    return FlashParam_LoadAll(&app, cfg);
}

int FlashParam_SaveNet(const AppNetConfig_t *cfg)
{
    AppConfig_t app;
    AppNetConfig_t old_net;
    int ret;

    if ((cfg == 0) || (!AppNetConfig_IsValid(cfg)))
    {
        return -1;
    }

    ret = FlashParam_LoadAll(&app, &old_net);
    if (ret != 0)
    {
        app = AppConfig_Default();
    }

    return FlashParam_SaveAll(&app, cfg);
}
