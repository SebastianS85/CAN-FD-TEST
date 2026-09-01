#ifndef APP_PERIPHERALS_H
#define APP_PERIPHERALS_H

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "ds3231mz.h"
#include "pcf8574.h"
#include "c_oled.h"

typedef struct {
    i2c_port_num_t port;
    gpio_num_t sda_pin;
    gpio_num_t scl_pin;
    uint32_t freq_hz;
    uint8_t oled_addr;
    uint8_t pcf8574_addr;
} app_peripherals_config_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    ds3231mz_t rtc;
    pcf8574_t pcf8574;
    bool oled_available;
} app_peripherals_handles_t;

esp_err_t app_peripherals_init(const app_peripherals_config_t *config);
app_peripherals_handles_t *app_peripherals_get_handles(void);
esp_err_t app_peripherals_set_datetime(uint16_t year, uint8_t month, uint8_t date,
                                       uint8_t day, uint8_t hour, uint8_t minute,
                                       uint8_t second);
esp_err_t sync_system_time_from_rtc(ds3231mz_t *rtc_dev);

#endif 
