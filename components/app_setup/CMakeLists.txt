idf_component_register(SRCS "app_setup.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_buffers app_peripherals app_stats_ui app_mode_state can_logger can_services esp_ringbuf sdcard_service tcp_service wifi_manager)