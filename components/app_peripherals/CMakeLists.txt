idf_component_register(SRCS "app_peripherals.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c c_oled ds3231mz pcf8574)
