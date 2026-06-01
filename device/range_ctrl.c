#include "range_ctrl.h"
#include "app_config.h"
#include "main.h"

/*
 * STM32F427VGT6 + AO6802 增益档位控制
 *
 * 结合当前原理图：
 *   PG11 -> Q5B -> 控制 CH1_D0
 *   PG12 -> Q5A -> 控制 CH1_D1
 *   PG13 -> Q6B -> 控制 CH1_D2
 *   PG14 -> Q6A -> 控制 CH1_D3
 *
 * 原理图逻辑：
 *   MCU PGx = 1：上方 AO6802 导通，把 CH1_Dx 拉到 0V，下面反馈支路关闭
 *   MCU PGx = 0：上方 AO6802 关闭，CH1_Dx 被 5V 上拉，下面反馈支路导通
 *
 * 所以：
 *   MCU GPIO 低电平 = 打开对应增益档
 *   MCU GPIO 高电平 = 关闭对应增益档
 *
 * range 字段继续保留原协议名字，但含义改为增益档：
 *   range = 0 -> 0dB  -> CH1_D0 -> R49/R50
 *   range = 1 -> 20dB -> CH1_D1 -> R51/R52
 *   range = 2 -> 40dB -> CH1_D2 -> R53/R54
 *   range = 3 -> 保留，不使用，始终关闭
 */

#ifndef RANGE_ALL_OFF_DELAY_MS
#define RANGE_ALL_OFF_DELAY_MS        5U
#endif

#ifndef RANGE_SETTLE_DELAY_MS
#define RANGE_SETTLE_DELAY_MS         50U
#endif

/* 当前硬件必须使用低电平打开目标档。 */
#define RANGE_GPIO_ON_LEVEL           GPIO_PIN_RESET
#define RANGE_GPIO_OFF_LEVEL          GPIO_PIN_SET

static uint8_t s_range = APP_GAIN_0DB;

static void range_write_ch0(GPIO_PinState st)
{
    HAL_GPIO_WritePin(CH1_D0_GPIO_Port, CH1_D0_Pin, st);
}

static void range_write_ch1(GPIO_PinState st)
{
    HAL_GPIO_WritePin(CH1_D1_GPIO_Port, CH1_D1_Pin, st);
}

static void range_write_ch2(GPIO_PinState st)
{
    HAL_GPIO_WritePin(CH1_D2_GPIO_Port, CH1_D2_Pin, st);
}

static void range_write_ch3(GPIO_PinState st)
{
    HAL_GPIO_WritePin(CH1_D3_GPIO_Port, CH1_D3_Pin, st);
}

void RangeCtrl_AllOff(void)
{
    /* 全部关闭：PG11~PG14 全部输出高电平，CH1_D0~D3 被拉低。 */
    range_write_ch0(RANGE_GPIO_OFF_LEVEL);
    range_write_ch1(RANGE_GPIO_OFF_LEVEL);
    range_write_ch2(RANGE_GPIO_OFF_LEVEL);
    range_write_ch3(RANGE_GPIO_OFF_LEVEL);
}

int RangeCtrl_Set(uint8_t range)
{
    if (!((range == APP_GAIN_0DB) ||
          (range == APP_GAIN_20DB) ||
          (range == APP_GAIN_40DB)))
    {
        RangeCtrl_AllOff();
        return -1;
    }

    /* 防止两个档位同时导通：先全部关闭，再打开目标档。 */
    RangeCtrl_AllOff();
    HAL_Delay(RANGE_ALL_OFF_DELAY_MS);

    switch (range)
    {
    case APP_GAIN_0DB:
        /* 0dB：PG11 输出低，CH1_D0 被上拉到 5V，R49/R50 支路导通。 */
        range_write_ch0(RANGE_GPIO_ON_LEVEL);
        break;

    case APP_GAIN_20DB:
        /* 20dB：PG12 输出低，CH1_D1 被上拉到 5V，R51/R52 支路导通。 */
        range_write_ch1(RANGE_GPIO_ON_LEVEL);
        break;

    case APP_GAIN_40DB:
        /* 40dB：PG13 输出低，CH1_D2 被上拉到 5V，R53/R54 支路导通。 */
        range_write_ch2(RANGE_GPIO_ON_LEVEL);
        break;

    default:
        RangeCtrl_AllOff();
        return -1;
    }

    /* D3 保留，始终关闭。 */
    range_write_ch3(RANGE_GPIO_OFF_LEVEL);

    s_range = range;
    HAL_Delay(RANGE_SETTLE_DELAY_MS);

    return 0;
}

uint8_t RangeCtrl_Get(void)
{
    return s_range;
}

uint8_t RangeCtrl_GetGainDb(void)
{
    return AppConfig_RangeToGainDb(s_range);
}

uint16_t RangeCtrl_GetGainX(void)
{
    return AppConfig_RangeToGainX(s_range);
}

const char *RangeCtrl_GetName(void)
{
    return AppConfig_RangeName(s_range);
}
