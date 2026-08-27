
#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    TWAI_BUS_MODE_CLASSIC,
    TWAI_BUS_MODE_FD
} twai_bus_mode_t;

typedef void (*twai_mgr_app_rx_cb_t)(uint8_t node_id, const twai_frame_t *frame, void *user_ctx);

typedef struct {
    gpio_num_t tx_io;
    gpio_num_t rx_io;
    twai_bus_mode_t mode;
    uint32_t arb_bitrate;
    uint32_t data_bitrate;
    uint32_t tx_queue_depth;
    void *user_ctx;
    twai_mgr_app_rx_cb_t app_rx_cb;
} twai_mgr_config_t;

typedef struct {
    twai_node_handle_t handle;
    twai_mgr_config_t current_cfg;
    int bad_state_streak;
    uint32_t recover_count;
    volatile bool recovery_in_progress;
} twai_mgr_inst_t;


esp_err_t twai_mgr_init_custom_node(twai_mgr_inst_t *inst, gpio_num_t tx_io, gpio_num_t rx_io, 
                                     twai_bus_mode_t mode, uint32_t arb_bitrate, uint32_t data_bitrate, 
                                     uint32_t queue_depth, void *user_ctx, twai_mgr_app_rx_cb_t rx_cb);


esp_err_t twai_mgr_init_fd_node(twai_mgr_inst_t *inst, gpio_num_t tx_io, gpio_num_t rx_io, uint32_t queue_depth, void *user_ctx, twai_mgr_app_rx_cb_t rx_cb);

esp_err_t twai_mgr_reconfigure(twai_mgr_inst_t *inst, const twai_mgr_config_t *new_cfg);
void twai_mgr_health_monitor(twai_mgr_inst_t *inst);
esp_err_t twai_mgr_async_transmit(twai_mgr_inst_t *inst, const twai_frame_t *tx_frame, TickType_t timeout_ticks);
esp_err_t twai_mgr_set_filter(twai_mgr_inst_t *inst, uint32_t id1, uint32_t mask1, uint32_t id2, uint32_t mask2, bool is_ext);

uint8_t twai_mgr_bound_dlc(uint8_t dlc);
uint8_t twai_mgr_dlc_to_len(uint8_t dlc);


