#include "flash_param.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_flash_ex.h"
#include <string.h>
#include <stddef.h>

/* STM32F427VGT6 1MB Flash：最后一个 128KB sector 起始地址 */
#define PARAM_FLASH_ADDR          0x080E0000UL
#define PARAM_FLASH_SECTOR        FLASH_SECTOR_11

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t fs_hz;
    uint8_t  bits;
    uint8_t  range;
    uint8_t  reserved0;
    uint8_t  reserved1;
    uint16_t crc16;
    uint16_t pad;
} FlashParamRecord_t;

static uint16_t crc16_modbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
    }
    return crc;
}

int FlashParam_Load(AppConfig_t *cfg)
{
    const FlashParamRecord_t *rec = (const FlashParamRecord_t *)PARAM_FLASH_ADDR;
    FlashParamRecord_t tmp;

    if (cfg == 0) return -1;
    memcpy(&tmp, rec, sizeof(tmp));

    uint16_t calc = crc16_modbus((const uint8_t *)&tmp, offsetof(FlashParamRecord_t, crc16));
    if ((tmp.magic != APP_PARAM_MAGIC) || (tmp.version != APP_PARAM_VERSION) || (tmp.crc16 != calc))
    {
        *cfg = AppConfig_Default();
        return -2;
    }

    cfg->fs_hz = tmp.fs_hz;
    cfg->bits = tmp.bits;
    cfg->range = tmp.range;
    cfg->reserved0 = 0;
    cfg->reserved1 = 0;

    if (!AppConfig_IsValid(cfg))
    {
        *cfg = AppConfig_Default();
        return -3;
    }

    return 0;
}

int FlashParam_Save(const AppConfig_t *cfg)
{
    if (!AppConfig_IsValid(cfg)) return -1;

    FlashParamRecord_t rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.magic = APP_PARAM_MAGIC;
    rec.version = APP_PARAM_VERSION;
    rec.fs_hz = cfg->fs_hz;
    rec.bits = cfg->bits;
    rec.range = cfg->range;
    rec.reserved0 = 0;
    rec.reserved1 = 0;
    rec.crc16 = crc16_modbus((const uint8_t *)&rec, offsetof(FlashParamRecord_t, crc16));

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = PARAM_FLASH_SECTOR;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -2;
    }

    const uint8_t *p = (const uint8_t *)&rec;
    for (uint32_t i = 0; i < sizeof(rec); i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, PARAM_FLASH_ADDR + i, p[i]) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return -3;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}
