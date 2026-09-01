#ifndef APP_BUFFERS_H
#define APP_BUFFERS_H

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_err.h"

typedef struct {
    size_t log_buf_size;
    size_t route_buf_size;
} app_buffers_config_t;

typedef struct {
    RingbufHandle_t log_ringbuf;
    RingbufHandle_t route_ringbuf;
} app_buffers_t;

esp_err_t app_buffers_init(const app_buffers_config_t *config, app_buffers_t *out_bufs);

#endif 
