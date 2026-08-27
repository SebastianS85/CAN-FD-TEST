idf_component_register(SRCS "app_stats_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c_oled twai_manager wifi_manager can_logger)
