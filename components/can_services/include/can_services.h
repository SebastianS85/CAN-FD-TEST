#ifndef CAN_SERVICES_H
#define CAN_SERVICES_H

#include "twai_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

typedef struct {
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    twai_bus_mode_t mode;
    uint32_t arbitration_bitrate;
    uint32_t data_bitrate;
    uint32_t tx_queue_depth;
    bool listen_only;
} can_services_node_config_t;

typedef struct {
    can_services_node_config_t node1_config;
    can_services_node_config_t node2_config;
    RingbufHandle_t log_ringbuf;
    RingbufHandle_t route_ringbuf;
    twai_mgr_inst_t *route_target;
    bool route_use_fd_frames;
    volatile uint32_t *route_tx_count;
    volatile uint32_t *log_drop_count;
    volatile uint32_t *route_drop_count;
    volatile uint32_t *rx_count;
    volatile uint32_t *rx_count_node1;
    volatile uint32_t *rx_count_node2;
    volatile uint32_t *log_ringbuf_in_count;
    bool enable_generator;
} can_services_config_t;

esp_err_t can_services_configure(const can_services_config_t *config);
esp_err_t can_services_init_nodes(void);
esp_err_t can_services_start(const can_services_config_t *config);
twai_mgr_inst_t *can_services_get_node(uint8_t node_id);
esp_err_t can_services_reconfigure_node(uint8_t node_id, uint32_t arbitration_bitrate,
                                        uint32_t data_bitrate, bool listen_only);
void can_services_rx_handler(uint8_t node_id, const twai_frame_t *rx_frame, void *user_ctx);
void can_services_set_bridge_frame_replace(bool enabled, bool filter_enabled, uint32_t filter_id,
                                           uint32_t new_id, uint8_t new_dlc,
                                           const uint8_t *new_data, size_t new_data_len);

#endif 
