#ifndef TEST_MAIN_H
#define TEST_MAIN_H
#include <stdint.h>
#define GPIOE 0
#define GPIO_PIN_3 8U
#define EXTI3_IRQn 0
#define __HAL_GPIO_EXTI_CLEAR_IT(pin) ((void)(pin))
static inline uint32_t __get_PRIMASK(void) { return 0U; }
static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline void HAL_NVIC_EnableIRQ(int irq) { (void)irq; }
static inline void HAL_NVIC_DisableIRQ(int irq) { (void)irq; }
static inline uint8_t HAL_GPIO_ReadPin(int port, uint16_t pin)
{
    (void)port;
    (void)pin;
    return 1U;
}
#endif
