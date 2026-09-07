#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t c_oled_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t c_oled_set_rotation_180(bool enabled);
void c_oled_clear_buffer(void);
void c_oled_draw_string(int x, int page, const char *str);
esp_err_t c_oled_update(void);

#ifdef __cplusplus
}
#endif