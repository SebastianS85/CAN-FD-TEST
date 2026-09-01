#ifndef TCP_SERVICE_H
#define TCP_SERVICE_H

#include <stdint.h>
#include "esp_err.h"
#include "freertos/ringbuf.h"
#include "twai_manager.h"

typedef struct {
    uint16_t tx_port;
    uint16_t rx_port;
    RingbufHandle_t ringbuf;
    twai_mgr_inst_t *node1;
    twai_mgr_inst_t *node2;
    bool use_fd_frames;
    volatile uint32_t *tx_frames_node1;
    volatile uint32_t *tx_frames_node2;
    volatile uint32_t *ringbuf_out;
} tcp_service_config_t;

esp_err_t tcp_service_start(const tcp_service_config_t *config);

#endif