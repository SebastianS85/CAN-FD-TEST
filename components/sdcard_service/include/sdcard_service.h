#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pin_mosi;
    int pin_miso;
    int pin_sclk;
    int pin_cs;
    spi_host_device_t host_id;
    bool format_if_mount_failed;
    int max_files;
    const char *mount_point;
} sdcard_service_config_t;

// Inicjalizacja i de-inicjalizacja
esp_err_t sdcard_service_mount(const sdcard_service_config_t *config, sdmmc_card_t **out_card);
esp_err_t sdcard_service_unmount(void);
esp_err_t sdcard_service_sync(void);

// Operacje tekstowe (np. do logów systemowych)
esp_err_t sdcard_service_write_text(const char *relative_path, const char *text);
esp_err_t sdcard_service_append_text(const char *relative_path, const char *text);
esp_err_t sdcard_service_read_text(const char *relative_path, char *buffer, size_t buffer_len);

// NOWA FUNKCJA: Superszybki zapis binarny (idealny dla zrzutów 4 KB ramek CAN)
esp_err_t sdcard_service_append_bin_block(const char *relative_path, const void *data, size_t size);

// Funkcje pomocnicze
esp_err_t sdcard_service_run_self_test(void);
const char *sdcard_service_mount_point(void);

#ifdef __cplusplus
}
#endif