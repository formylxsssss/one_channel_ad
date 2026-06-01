#ifndef FLASH_PARAM_H
#define FLASH_PARAM_H

#include "app_config.h"
#include "app_net_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 兼容旧接口：
 *   FlashParam_Load / FlashParam_Save 仍只操作采样参数 AppConfig_t。
 *
 * 新接口：
 *   FlashParam_LoadAll / FlashParam_SaveAll 同时操作采样参数 + 网络参数。
 *   FlashParam_LoadNet / FlashParam_SaveNet 只操作网络参数，但保存时会保留当前采样参数。
 */
int FlashParam_Load(AppConfig_t *cfg);
int FlashParam_Save(const AppConfig_t *cfg);

int FlashParam_LoadAll(AppConfig_t *cfg, AppNetConfig_t *net);
int FlashParam_SaveAll(const AppConfig_t *cfg, const AppNetConfig_t *net);

int FlashParam_LoadNet(AppNetConfig_t *cfg);
int FlashParam_SaveNet(const AppNetConfig_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
