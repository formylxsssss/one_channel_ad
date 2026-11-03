#include "ads127l11.h"

static uint8_t s_test_bps = 3U;

int ADS127L11_PrepareContinuousDma(const AppConfig_t *cfg)
{
    s_test_bps = AppConfig_BytesPerSample(cfg);
    return 0;
}
void ADS127L11_Stop(void) {}
void ADS127L11_Start(void) {}
void ADS127L11_StreamEnter(void) {}
void ADS127L11_StreamExit(void) {}
void ADS127L11_StopContinuousDma(void) {}
void ADS127L11_PollContinuousDma(void) {}
int ADS127L11_StartContinuousDmaFromDrdyIsr(void) { return 0; }
uint8_t ADS127L11_GetContinuousFrameBytes(void) { return s_test_bps + 1U; }
uint8_t ADS127L11_GetContinuousDataOffset(void) { return 1U; }
uint8_t ADS127L11_GetContinuousBytesPerSample(void) { return s_test_bps; }
