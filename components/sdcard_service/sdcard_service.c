#include "sdcard_service.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdcard_service";

static bool s_mounted;
static int s_host_id = -1;
static sdmmc_card_t *s_card;
static char s_mount_point[32] = "/sdcard";
static FILE *s_append_file;
static char s_append_path[96];

static esp_err_t build_full_path(const char *relative_path, char *full_path, size_t full_path_len)
{
    ESP_RETURN_ON_FALSE(relative_path != NULL, ESP_ERR_INVALID_ARG, TAG, "relative_path is null");
    ESP_RETURN_ON_FALSE(full_path != NULL, ESP_ERR_INVALID_ARG, TAG, "full_path is null");
    ESP_RETURN_ON_FALSE(relative_path[0] == '/', ESP_ERR_INVALID_ARG, TAG, "path must start with '/'");

    int written = snprintf(full_path, full_path_len, "%s%s", s_mount_point, relative_path);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < full_path_len, ESP_ERR_INVALID_SIZE, TAG, "full path too long");
    return ESP_OK;
}

esp_err_t sdcard_service_mount(const sdcard_service_config_t *config, sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(config->mount_point != NULL, ESP_ERR_INVALID_ARG, TAG, "mount_point is null");

    if (s_mounted) {
        if (out_card) {
            *out_card = s_card;
        }
        return ESP_OK;
    }

   
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = config->pin_mosi,
        .miso_io_num = config->pin_miso,
        .sclk_io_num = config->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = config->max_transfer_sz,
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->host_id, &bus_cfg, SDSPI_DEFAULT_DMA), TAG, "spi_bus_initialize failed");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = config->host_id;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = config->host_id;
    slot_config.gpio_cs = config->pin_cs;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = config->format_if_mount_failed,
        .max_files = config->max_files,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdspi_mount(config->mount_point, &host, &slot_config, &mount_cfg, &card);
    if (err != ESP_OK) {
        spi_bus_free(config->host_id);
        return err;
    }

    s_mounted = true;
    s_host_id = config->host_id;
    s_card = card;
    strlcpy(s_mount_point, config->mount_point, sizeof(s_mount_point));

    ESP_LOGI(TAG, "SD card mounted at %s", s_mount_point);
    sdmmc_card_print_info(stdout, s_card);

    if (out_card) {
        *out_card = s_card;
    }

    return ESP_OK;
}

esp_err_t sdcard_service_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    if (s_append_file != NULL) {
        fclose(s_append_file);
        s_append_file = NULL;
        s_append_path[0] = '\0';
    }

    esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    ESP_RETURN_ON_ERROR(spi_bus_free(s_host_id), TAG, "spi_bus_free failed");

    s_mounted = false;
    s_host_id = -1;
    s_card = NULL;
    return ESP_OK;
}

esp_err_t sdcard_service_write_text(const char *relative_path, const char *text)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "sdcard not mounted");
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is null");

    char path[96];
    ESP_RETURN_ON_ERROR(build_full_path(relative_path, path, sizeof(path)), TAG, "invalid path");

    FILE *f = fopen(path, "w");
    ESP_RETURN_ON_FALSE(f != NULL, ESP_FAIL, TAG, "fopen for write failed");

    fputs(text, f);
    fclose(f);
    return ESP_OK;
}

esp_err_t sdcard_service_append_text(const char *relative_path, const char *text)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "sdcard not mounted");
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is null");

    char path[96];
    ESP_RETURN_ON_ERROR(build_full_path(relative_path, path, sizeof(path)), TAG, "invalid path");

    if (s_append_file == NULL || strcmp(s_append_path, path) != 0) {
        if (s_append_file != NULL) {
            fclose(s_append_file);
        }

        s_append_file = fopen(path, "a");
        ESP_RETURN_ON_FALSE(s_append_file != NULL, ESP_FAIL, TAG, "fopen for append failed");
        strlcpy(s_append_path, path, sizeof(s_append_path));
    }

    ESP_RETURN_ON_FALSE(fputs(text, s_append_file) >= 0, ESP_FAIL, TAG, "append write failed");
    ESP_RETURN_ON_FALSE(fflush(s_append_file) == 0, ESP_FAIL, TAG, "append flush failed");
    return ESP_OK;
}

esp_err_t sdcard_service_read_text(const char *relative_path, char *buffer, size_t buffer_len)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "sdcard not mounted");
    ESP_RETURN_ON_FALSE(buffer != NULL && buffer_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid buffer");

    char path[96];
    ESP_RETURN_ON_ERROR(build_full_path(relative_path, path, sizeof(path)), TAG, "invalid path");

    FILE *f = fopen(path, "r");
    ESP_RETURN_ON_FALSE(f != NULL, ESP_FAIL, TAG, "fopen for read failed");

    size_t rd = fread(buffer, 1, buffer_len - 1, f);
    buffer[rd] = '\0';
    fclose(f);
    return ESP_OK;
}

esp_err_t sdcard_service_run_self_test(void)
{
    static const char *k_test_file = "/health.txt";
    static const char *k_text = "sdcard_service self test\n";

    char readback[64];

    ESP_RETURN_ON_ERROR(sdcard_service_write_text(k_test_file, k_text), TAG, "write self-test failed");
    ESP_RETURN_ON_ERROR(sdcard_service_read_text(k_test_file, readback, sizeof(readback)), TAG, "read self-test failed");

    ESP_RETURN_ON_FALSE(strncmp(readback, k_text, strlen(k_text)) == 0, ESP_FAIL, TAG, "self-test content mismatch");
    ESP_LOGI(TAG, "Self-test PASS (%s%s)", s_mount_point, k_test_file);
    return ESP_OK;
}

const char *sdcard_service_mount_point(void)
{
    return s_mount_point;
}