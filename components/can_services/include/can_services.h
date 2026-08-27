#ifndef CAN_SERVICES_H
#define CAN_SERVICES_H

#include "twai_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

typedef struct {
    twai_mgr_inst_t *node1;
    twai_mgr_inst_t *node2;
    RingbufHandle_t route_ringbuf;
    bool enable_generator;
} can_services_config_t;

esp_err_t can_services_start(const can_services_config_t *config);

#endif // CAN_SERVICES_H
