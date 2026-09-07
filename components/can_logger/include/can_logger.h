#ifndef CAN_LOGGER_H
#define CAN_LOGGER_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "sdcard_service.h"
#include "ds3231mz.h"
#include "app_types.h"

typedef struct {
    gpio_num_t eject_button_pin;
    RingbufHandle_t ringbuf;
    volatile uint32_t *ringbuf_out_count;
    ds3231mz_t *rtc_dev;
    sdcard_service_config_t sd_hw_config;
} can_logger_config_t;

esp_err_t can_logger_init(const can_logger_config_t *config);
void can_logger_request_stop(void);
bool can_logger_is_stop_requested(void);
bool can_logger_is_sd_card_mounted(void);

#endif 
