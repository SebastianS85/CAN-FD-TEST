idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_buffers app_peripherals app_types app_stats_ui can_logger can_services esp_ringbuf esp_wifi esp_netif lwip nvs_flash tcp_service twai_manager wifi_manager)
