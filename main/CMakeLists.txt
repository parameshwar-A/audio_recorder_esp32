idf_component_register(SRCS "inmp441_audio_recorder.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_touch_sens esp_driver_uart)
