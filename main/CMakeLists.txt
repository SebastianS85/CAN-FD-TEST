idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ds3231mz esp_ringbuf pcf8574 esp_driver_i2c  sdcard_service c_oled esp_wifi esp_netif lwip nvs_flash twai_manager)
