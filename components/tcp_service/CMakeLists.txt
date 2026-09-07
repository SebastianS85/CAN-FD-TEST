idf_component_register(SRCS "tcp_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lwip wifi_manager twai_manager can_services app_types app_mode_state esp_ringbuf)