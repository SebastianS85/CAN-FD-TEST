#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231MZ_I2C_ADDR_DEFAULT 0x68

typedef struct {
    uint16_t year;      
    uint8_t month;      
    uint8_t date;       
    uint8_t day;        
    uint8_t hour;       
    uint8_t minute;     
    uint8_t second;     
} ds3231mz_datetime_t;

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    uint8_t i2c_addr;
    uint32_t scl_speed_hz;
    int timeout_ms;
} ds3231mz_t;

esp_err_t ds3231mz_init(ds3231mz_t *dev,
                        i2c_master_bus_handle_t bus_handle,
                        uint8_t i2c_addr,
                        uint32_t scl_speed_hz,
                        int timeout_ms);

esp_err_t ds3231mz_deinit(ds3231mz_t *dev);

esp_err_t ds3231mz_read_time(ds3231mz_t *dev, ds3231mz_datetime_t *dt);
esp_err_t ds3231mz_set_time(ds3231mz_t *dev, const ds3231mz_datetime_t *dt);

esp_err_t ds3231mz_read_temperature(ds3231mz_t *dev, float *temp_c);

esp_err_t ds3231mz_get_power_lost(ds3231mz_t *dev, bool *power_lost);
esp_err_t ds3231mz_clear_power_lost(ds3231mz_t *dev);

#ifdef __cplusplus
}
#endif
