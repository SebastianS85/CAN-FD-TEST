#include "app_buffers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "app_buffers";

static RingbufHandle_t create_psram_ringbuf(size_t size) {
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    StaticRingbuffer_t *struct_ptr = heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf && struct_ptr) {
        return xRingbufferCreateStatic(size, RINGBUF_TYPE_NOSPLIT, buf, struct_ptr);
    }
    heap_caps_free(buf);
    heap_caps_free(struct_ptr);
    return NULL;
}

esp_err_t app_buffers_init(const app_buffers_config_t *config, app_buffers_t *out_bufs) {
    if (!config || !out_bufs) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Allocating Ringbuffers in PSRAM...");
    out_bufs->log_ringbuf = config->log_buf_size ? create_psram_ringbuf(config->log_buf_size) : NULL;
    out_bufs->route_ringbuf = config->route_buf_size ? create_psram_ringbuf(config->route_buf_size) : NULL;

    if ((config->log_buf_size && !out_bufs->log_ringbuf) ||
        (config->route_buf_size && !out_bufs->route_ringbuf)) {
        ESP_LOGE(TAG, "FATAL: Not enough PSRAM for CAN buffers!");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
