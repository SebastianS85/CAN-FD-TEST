idf_component_register(SRCS "twai_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_twai freertos esp_driver_gpio )
