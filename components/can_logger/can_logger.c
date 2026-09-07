#include <stdio.h>
#include <string.h>
#include "can_logger.h"
#include "app_mode_state.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"

static const char *TAG = "can_logger";
#define SD_BLOCK_SIZE (16 * 1024)

static volatile bool s_stop_logging = false;
static volatile bool s_sd_card_mounted = false;
static RingbufHandle_t s_sd_ringbuf = NULL;
static volatile uint32_t *s_ringbuf_out_count = NULL;
static ds3231mz_t *s_rtc_dev = NULL;
static sdcard_service_config_t s_sd_config = {0};

void can_logger_request_stop(void) {
    s_stop_logging = true;
}

bool can_logger_is_stop_requested(void) {
    return s_stop_logging;
}

bool can_logger_is_sd_card_mounted(void) {
    return s_sd_card_mounted;
}

static void IRAM_ATTR eject_btn_isr_handler(void* arg) {
    s_stop_logging = true;
}

static void sd_writer_task(void *pvParameters) {
    uint8_t *write_buffer = heap_caps_malloc(SD_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!write_buffer) {
        ESP_LOGE(TAG, "Failed to allocate SD write buffer!");
        vTaskDelete(NULL);
        return;
    }
    size_t current_bytes = 0;
    size_t item_size;

    char log_filename[64] = "/can_log.bin";

    while (1) {
        if (app_mode_state_get_mode() != APP_DISPLAY_MODE_SD_LOGGER) {
            /* Not the active mode (remote mode switched away): flush and unmount
             * so the card sits idle until SD logger mode is selected again. */
            if (s_sd_card_mounted) {
                if (current_bytes > 0) {
                    sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes);
                    current_bytes = 0;
                }
                sdcard_service_sync();
                sdcard_service_unmount();
                s_sd_card_mounted = false;
                ESP_LOGI(TAG, "SD logging paused (mode switched away)");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (s_stop_logging) {
            if (s_sd_card_mounted && current_bytes > 0) {
                sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes);
            }
            ESP_LOGW(TAG, "Closing file and unmounting card...");

            if (s_sd_card_mounted) {
                sdcard_service_sync();
                sdcard_service_unmount();
                s_sd_card_mounted = false;
            }
            
            ESP_LOGI(TAG, "== SD CARD IS SAFE! YOU CAN REMOVE IT ==");
            if (write_buffer) free(write_buffer);
            vTaskDelete(NULL);        
        }

        if (!s_sd_card_mounted) {
            sdmmc_card_t *card = NULL;
            if (sdcard_service_mount(&s_sd_config, &card) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ds3231mz_datetime_t now = {0};
            if (s_rtc_dev && ds3231mz_read_time(s_rtc_dev, &now) == ESP_OK) {
                snprintf(log_filename, sizeof(log_filename), "/log_%04u%02u%02u_%02u%02u%02u.bin",
                         now.year, now.month, now.date, now.hour, now.minute, now.second);
            }
            s_sd_card_mounted = true;
            ESP_LOGI(TAG, "SD card ready, logging to %s", log_filename);
        }

        void *data = xRingbufferReceive(s_sd_ringbuf, &item_size, pdMS_TO_TICKS(1000));
        
        if (data != NULL) {
            if (s_ringbuf_out_count) {
                (*s_ringbuf_out_count)++;
            }
            if (current_bytes + item_size > SD_BLOCK_SIZE) {
                if (sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes) != ESP_OK) {
                    s_sd_card_mounted = false;
                    sdcard_service_unmount();
                }
                current_bytes = 0;
            }
            
            memcpy(write_buffer + current_bytes, data, item_size);
            current_bytes += item_size;
            vRingbufferReturnItem(s_sd_ringbuf, data);
        } 
        else {
            if (current_bytes > 0) {
                if (sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes) != ESP_OK) {
                    s_sd_card_mounted = false;
                    sdcard_service_unmount();
                }
                current_bytes = 0;
            }
            if (s_sd_card_mounted && sdcard_service_sync() != ESP_OK) {
                s_sd_card_mounted = false;
                sdcard_service_unmount();
            }
        }
    }
}

esp_err_t can_logger_init(const can_logger_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!config->ringbuf) {
        ESP_LOGI(TAG, "SD Card logging disabled (no ringbuf).");
        return ESP_OK;
    }

    s_sd_ringbuf = config->ringbuf;
    s_ringbuf_out_count = config->ringbuf_out_count;
    s_rtc_dev = config->rtc_dev;
    s_sd_config = config->sd_hw_config;
    s_sd_card_mounted = false;

    if (config->eject_button_pin != GPIO_NUM_NC) {
        gpio_config_t btn_conf = {
            .intr_type = GPIO_INTR_NEGEDGE,
            .pin_bit_mask = (1ULL << config->eject_button_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = 1,
            .pull_down_en = 0
        };
        gpio_config(&btn_conf);
        gpio_install_isr_service(0);
        gpio_isr_handler_add(config->eject_button_pin, eject_btn_isr_handler, NULL);
    }

    return xTaskCreatePinnedToCore(sd_writer_task, "sd_tx", 4096, NULL, 2, NULL, 0) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
