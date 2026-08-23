#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF8574_I2C_ADDR_DEFAULT 0x20
#define PCF8574_PIN_COUNT 8

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    uint8_t i2c_addr;
    uint8_t output_latch;
    int timeout_ms;
} pcf8574_t;

esp_err_t pcf8574_init(pcf8574_t *dev,
                       i2c_master_bus_handle_t bus_handle,
                       uint8_t i2c_addr,
                       uint32_t scl_speed_hz,
                       int timeout_ms,
                       uint8_t initial_value);

esp_err_t pcf8574_deinit(pcf8574_t *dev);
esp_err_t pcf8574_write_byte(pcf8574_t *dev, uint8_t value);
esp_err_t pcf8574_read_byte(pcf8574_t *dev, uint8_t *value);
esp_err_t pcf8574_write_pin(pcf8574_t *dev, uint8_t pin, bool high);
esp_err_t pcf8574_read_pin(pcf8574_t *dev, uint8_t pin, bool *high);
esp_err_t pcf8574_toggle_pin(pcf8574_t *dev, uint8_t pin);

#ifdef __cplusplus
}
#endif
