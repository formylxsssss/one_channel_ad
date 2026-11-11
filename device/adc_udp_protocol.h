#ifndef ADC_UDP_PROTOCOL_H
#define ADC_UDP_PROTOCOL_H
#include "app_config.h"

#define APP_UDP_MAX_PAYLOAD 1252U
#define APP_UDP_COMMAND_MAX 256U

uint16_t AdcUdp_Crc16(const uint8_t *data, uint32_t len);
uint16_t AdcUdp_MaxSamples(uint16_t mtu, uint8_t bps);
uint16_t AdcUdp_BuildData(uint8_t *out, uint16_t capacity, uint32_t frame_seq,
                        const AppConfig_t *cfg, uint32_t first_sample,
                        const uint8_t *samples, uint16_t count, uint8_t bps);
int AdcUdp_ValidateCommand(const uint8_t *frame, uint16_t len);
#endif
