idf_component_register(SRCS "can_logger.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sdcard_service ds3231mz esp_ringbuf driver app_types heap)
