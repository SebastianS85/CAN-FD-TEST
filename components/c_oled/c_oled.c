#include <string.h>
#include "esp_log.h"
#include "c_oled.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "font8x8.h"

static const char *TAG = "C_OLED";

#define OLED_I2C_ADDR 0x3C
#define OLED_H_RES 128
#define OLED_V_RES 64

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint8_t s_oled_buffer[OLED_H_RES * OLED_V_RES / 8];

static void c_oled_draw_char(int x, int page, char c) {
    if (c < 32 || c > 127 || x > 120 || page > 7) return;
    int font_idx = (c - 32) * 8;
    for (int i = 0; i < 8; i++) {
        s_oled_buffer[(page * 128) + x + i] = font8x8[font_idx + i];
    }
}

esp_err_t c_oled_init(i2c_master_bus_handle_t i2c_bus) {
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "Brak uchwytu magistrali I2C!");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Inicjalizacja panelu IO dla SSD1306...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    
    esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Inicjalizacja sterownika SSD1306...");
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    
    ret = esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) return ret;

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    
    // Obrót o 180 stopni
    esp_lcd_panel_mirror(s_panel_handle, true, true);
    
    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    
    ESP_LOGI(TAG, "Gotowe!");
    return ESP_OK;
}

void c_oled_clear_buffer(void) {
    memset(s_oled_buffer, 0, sizeof(s_oled_buffer));
}

void c_oled_draw_string(int x, int page, const char *str) {
    while (*str) {
        if (x > 120) break;
        c_oled_draw_char(x, page, *str);
        x += 8;
        str++;
    }
}

esp_err_t c_oled_update(void) {
    if (s_panel_handle == NULL) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, OLED_H_RES, OLED_V_RES, s_oled_buffer);
}