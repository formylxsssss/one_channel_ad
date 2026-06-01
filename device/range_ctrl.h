#ifndef RANGE_CTRL_H
#define RANGE_CTRL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * range 字段现在表示前端增益档位：
 *   0 -> 0dB
 *   1 -> 20dB
 *   2 -> 40dB
 *   3 -> 保留，不允许正常使用
 */
int RangeCtrl_Set(uint8_t range);
uint8_t RangeCtrl_Get(void);
uint8_t RangeCtrl_GetGainDb(void);
uint16_t RangeCtrl_GetGainX(void);
const char *RangeCtrl_GetName(void);
void RangeCtrl_AllOff(void);

#ifdef __cplusplus
}
#endif

#endif
