idf_component_register(SRCS "udp_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lwip wifi_manager twai_manager app_types esp_ringbuf)
