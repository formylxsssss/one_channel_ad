TARGET := eth_tcp_client
BUILD := build/eth_tcp_client
TOOL := $(if $(GCC_PATH),$(GCC_PATH)/,)arm-none-eabi-
CC := $(TOOL)gcc
OBJCOPY := $(TOOL)objcopy
SIZE := $(TOOL)size
HAL := Drivers/STM32F4xx_HAL_Driver/Src
LWIP := Middlewares/Third_Party/LwIP/src
SOURCES := demos/eth_tcp_client/main.c demos/eth_tcp_client/selftest.c \
 Core/Src/system_stm32f4xx.c Core/Src/stm32f4xx_hal_msp.c \
 Core/Src/syscalls.c Core/Src/sysmem.c LWIP/Target/ethernetif.c \
 $(HAL)/stm32f4xx_hal.c $(HAL)/stm32f4xx_hal_cortex.c \
 $(HAL)/stm32f4xx_hal_rcc.c $(HAL)/stm32f4xx_hal_rcc_ex.c \
 $(HAL)/stm32f4xx_hal_pwr.c $(HAL)/stm32f4xx_hal_pwr_ex.c \
 $(HAL)/stm32f4xx_hal_gpio.c $(HAL)/stm32f4xx_hal_eth.c \
 ARM_SEGGER_RTT/SEGGER_RTT.c ARM_SEGGER_RTT/SEGGER_RTT_printf.c \
 $(wildcard $(LWIP)/core/*.c $(LWIP)/core/ipv4/*.c) \
 $(LWIP)/netif/ethernet.c
INCLUDES := Core/Inc Drivers/STM32F4xx_HAL_Driver/Inc \
 Drivers/STM32F4xx_HAL_Driver/Inc/Legacy Drivers/CMSIS/Include \
 Drivers/CMSIS/Device/ST/STM32F4xx/Include LWIP/Target \
 $(LWIP)/include Middlewares/Third_Party/LwIP/system ARM_SEGGER_RTT
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS := $(CPU) -DUSE_HAL_DRIVER -DSTM32F427xx $(addprefix -I,$(INCLUDES)) \
 -Og -g3 -Wall -ffunction-sections -fdata-sections -MMD -MP
OBJECTS := $(addprefix $(BUILD)/,$(notdir $(SOURCES:.c=.o)))
OBJECTS += $(BUILD)/startup_stm32f427xx.o
vpath %.c $(sort $(dir $(SOURCES)))
.PHONY: all
all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: demos/eth_tcp_client/main.c demos/eth_tcp_client/Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.c demos/eth_tcp_client/Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/startup_stm32f427xx.o: startup_stm32f427xx.s | $(BUILD)
	$(CC) $(CPU) -c $< -o $@
$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(CPU) -specs=nano.specs -TSTM32F427XX_FLASH.ld $^ \
	 -Wl,--gc-sections,-Map=$(BUILD)/$(TARGET).map \
	 -Wl,--wrap=HAL_ETH_Transmit,--wrap=HAL_ETH_ReadData -lc -lm -lnosys -o $@
	$(SIZE) $@
$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@
$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) --gap-fill 0xFF -O binary $< $@
-include $(OBJECTS:.o=.d)
