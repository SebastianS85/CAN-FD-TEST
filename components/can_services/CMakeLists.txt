idf_component_register(SRCS "can_services.c"
                       INCLUDE_DIRS "include"
                       REQUIRES twai_manager wifi_manager can_logger app_stats_ui app_types esp_ringbuf)
