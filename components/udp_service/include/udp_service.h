#ifndef UDP_SERVICE_H
#define UDP_SERVICE_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "twai_manager.h"
#include "app_types.h"

typedef struct {
    const char *host_ip;
    uint16_t tx_port;
    uint16_t rx_port;
    RingbufHandle_t tx_ringbuf;
    twai_mgr_inst_t *node1;
    twai_mgr_inst_t *node2;
    uint8_t use_fd_frames;
} udp_service_config_t;

esp_err_t udp_service_start(const udp_service_config_t *config);
uint32_t udp_service_get_tx_ok_count(void);

#endif // UDP_SERVICE_H
