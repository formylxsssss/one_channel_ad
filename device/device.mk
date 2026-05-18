DEVICE_SRC = device/adc_stream.c\
			device/adc_tcp_server.c\
			device/ads127l11.c\
			device/app_config.c\
			device/flash_param.c\
			device/range_ctrl.c\


# Required include directories
DEVICE_INC = device

ALLCSRC += $(DEVICE_SRC)
ALLINC += $(DEVICE_INC)
