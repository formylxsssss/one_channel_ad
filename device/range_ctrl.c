#include "range_ctrl.h"
#include "app_config.h"
#include "main.h"

static uint8_t s_range = APP_RANGE_D0_1K;

int RangeCtrl_Set(uint8_t range)
{
    if (range > APP_RANGE_D3_1M) return -1;

    HAL_GPIO_WritePin(CH1_D0_GPIO_Port, CH1_D0_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CH1_D1_GPIO_Port, CH1_D1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CH1_D2_GPIO_Port, CH1_D2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CH1_D3_GPIO_Port, CH1_D3_Pin, GPIO_PIN_RESET);

    switch (range)
    {
    case APP_RANGE_D0_1K:
        HAL_GPIO_WritePin(CH1_D0_GPIO_Port, CH1_D0_Pin, GPIO_PIN_SET);
        break;
    case APP_RANGE_D1_10K:
        HAL_GPIO_WritePin(CH1_D1_GPIO_Port, CH1_D1_Pin, GPIO_PIN_SET);
        break;
    case APP_RANGE_D2_100K:
        HAL_GPIO_WritePin(CH1_D2_GPIO_Port, CH1_D2_Pin, GPIO_PIN_SET);
        break;
    case APP_RANGE_D3_1M:
        HAL_GPIO_WritePin(CH1_D3_GPIO_Port, CH1_D3_Pin, GPIO_PIN_SET);
        break;
    default:
        return -1;
    }

    s_range = range;
    return 0;
}

uint8_t RangeCtrl_Get(void)
{
    return s_range;
}
