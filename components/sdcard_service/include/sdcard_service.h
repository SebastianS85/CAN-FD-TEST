#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int host_id;
    gpio_num_t pin_mosi;
    gpio_num_t pin_miso;
    gpio_num_t pin_sclk;
    gpio_num_t pin_cs;
    const char *mount_point;
    size_t max_files;
    bool format_if_mount_failed;
    uint32_t max_transfer_sz;
} sdcard_service_config_t;

#define SDCARD_SERVICE_DEFAULT_CONFIG() {      \
    .host_id = 1,                              \
    .pin_mosi = GPIO_NUM_8,                   \
    .pin_miso = GPIO_NUM_9,                   \
    .pin_sclk = GPIO_NUM_10,                   \
    .pin_cs = GPIO_NUM_6,                     \
    .mount_point = "/sdcard",                 \
    .max_files = 5,                            \
    .format_if_mount_failed = false,           \
    .max_transfer_sz = 4000,                   \
}

esp_err_t sdcard_service_mount(const sdcard_service_config_t *config, sdmmc_card_t **out_card);
esp_err_t sdcard_service_unmount(void);
esp_err_t sdcard_service_write_text(const char *relative_path, const char *text);
esp_err_t sdcard_service_append_text(const char *relative_path, const char *text);
esp_err_t sdcard_service_read_text(const char *relative_path, char *buffer, size_t buffer_len);
esp_err_t sdcard_service_run_self_test(void);
const char *sdcard_service_mount_point(void);

#ifdef __cplusplus
}
#endif
