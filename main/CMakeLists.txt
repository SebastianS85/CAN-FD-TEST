idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_setup can_services c_oled nvs_flash)
