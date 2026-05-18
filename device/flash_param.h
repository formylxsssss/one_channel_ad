#ifndef FLASH_PARAM_H
#define FLASH_PARAM_H

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

int FlashParam_Load(AppConfig_t *cfg);
int FlashParam_Save(const AppConfig_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
