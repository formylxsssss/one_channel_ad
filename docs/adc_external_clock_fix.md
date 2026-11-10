# ADC external-clock acquisition fix

## Changes

- The supplied CPU schematic connects ADC CLK through R8 to PB8 / TIM4_CH3.
  The old PA6 / TIM3_CH1 definition did not match this connection.
- Use the existing 25 MHz MCU HSE with PLL M=25, N=300, P=2: HCLK=150 MHz,
  APB1=37.5 MHz, APB2=75 MHz. TIMPRE=1 gives TIM4 a 150 MHz kernel clock.
  TIM4 PSC=0, ARR=5, CCR3=3 generates a 25 MHz, 50%-duty clock on PB8.
- Start this clock before ADC initialization. CONFIG4.CLK_SEL=1 selects it.
  SPI remains mode 1; select its divider using the actual APB2 clock.
- Keep command/Flash sample-rate selectors unchanged for compatibility. DATA
  metadata reports the actual rate, rounded to the nearest integer SPS. The
  existing 100 Hz DC-removal filter also uses this actual rate.
- Remove the previously added ADC-status/rate rejection and automatic stop
  mechanisms. This change fixes the acquisition clock, not the spectrum display.
- Retain READY data on temporary TCP send-buffer/queue pressure; do not discard
  it immediately. The finite ADC buffer can still overflow if sustained network
  throughput is insufficient. Existing ACK timeout/reconnection behavior remains.
- Compile only adc_stream.c and adc_tcp_server.c with -O2. Their previous -Og
  build could not sustain the highest rate after adding sample-by-sample DSP.
  The remaining modules retain the original optimization setting.

## Rates

fDATA = 25,000,000 / (2 * OSR). No samples are fabricated, duplicated, resampled,
or replaced with a generated waveform. The existing DC-removal processing remains.

| Command selector | OSR | Actual SPS | DATA fs_hz |
| --- | --- | --- | --- |
| 25000 | 512 | 24414.0625 | 24414 |
| 50000 | 256 | 48828.125 | 48828 |
| 100000 | 128 | 97656.25 | 97656 |
| 200000 | 64 | 195312.5 | 195313 |
| 400000 | 32 | 390625 | 390625 |

Upper-computer timing and FFT must use each DATA frame's fs_hz, not the command
selector or byte arrival rate. The current GUI already uses the DATA value.
ACK/REGISTER/GET_PARAM continue to carry the command selector; do not feed the
rounded DATA value back into SET_PARAM, which still accepts the five selectors.

## Board validation

Tested on 2026-11-04, STM32 UID 00400023 35325116 36383231, with the existing
10 kHz, 1 Vpp source and GUI/bridge. At the 50000 selector, the captured FFT peak
changed from approximately 8329.77 Hz (internal clock) to 10000.14 Hz (external
clock). Each FFT used actual received samples; there was no frequency correction
based on assuming the input tone.

The optimized 400000/24-bit build captured 2,364,470 consecutive samples with zero
CRC errors and zero sample-sequence gaps. A subsequent live query after more than
20 million conversions still reported overrun=0. Additional rate/width checks are
recorded under the ignored build/live-diagnostics directory.

The Thumb-code DC-removal tests cover all five selectors, 16/24-bit signed data,
extremes, chunk boundaries, restart, old STATUS frames, backpressure drops, and
100/1000 Hz filter response using the actual rate.

## Remaining Hardware Qualification

ADC STATUS.ADC_ERR and MOD_FLAG were still observed in raw status bytes after
switching clocks. No claim is made that the ADC meets datasheet accuracy, noise,
or distortion specifications. An external clock fixes the measured frequency
scale, but cannot repair a defective ADC or analog path. Power-cycle the board
and investigate any persistent ADC_ERR; do not hide harmonics or clear software
counters to present the board as fully healthy.

The user subsequently power-cycled the board. Boot readback still showed
STATUS=0xE4 and CONFIG4=0x81. A new 50000/24-bit capture contained 284,400 samples,
zero CRC errors, zero sequence gaps, and the same 10000.14 Hz peak. A temporary
MUX=2 internal-input-short test also returned STATUS=0xE7 during conversions:
ADC_ERR and MOD_FLAG persist even with external analog inputs disconnected by
the internal multiplexer. This localizes a remaining issue toward the ADC or its
reference/power environment; it is not proof of which physical component failed.
MUX=0 and normal 50000/24-bit streaming were restored after the test.

References: TI ADS127L11 datasheet, sections 6.8, 8.3.3, and 8.6.3:
https://www.ti.com/lit/ds/symlink/ads127l11.pdf

The .ioc pin, timer, and PLL settings are updated. After any CubeMX regeneration,
preserve TIMPRE=1 in the TIM4_Init 1 user block and the custom ADC/TCP initialization; regeneration was
not exercised as part of this fix. USB/SDIO/RNG PLLQ consumers are not used in the
current application and are not qualified by this change.
