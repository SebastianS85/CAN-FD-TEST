idf_component_register(SRCS "sdcard_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fatfs sdmmc esp_driver_sdspi esp_driver_spi)
