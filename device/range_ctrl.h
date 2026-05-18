#ifndef RANGE_CTRL_H
#define RANGE_CTRL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int RangeCtrl_Set(uint8_t range);
uint8_t RangeCtrl_Get(void);

#ifdef __cplusplus
}
#endif

#endif
