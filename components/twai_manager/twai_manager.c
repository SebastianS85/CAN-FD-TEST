#include "twai_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "twai_mgr";
#define TWAI_BAD_STATE_THRESHOLD 3
#define TWAI_RECOVERY_GUARD_DELAY_MS 20

static IRAM_ATTR bool twai_internal_rx_cb(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx) {
    twai_mgr_inst_t *inst = (twai_mgr_inst_t *)user_ctx;
    if (!inst || !inst->current_cfg.app_rx_cb) return false;

    uint8_t recv_buff[64];
    twai_frame_t rx_frame = { .buffer = recv_buff, .buffer_len = sizeof(recv_buff) };

    if (ESP_OK == twai_node_receive_from_isr(handle, &rx_frame)) {
       
        uint8_t node_id = (uint8_t)(uintptr_t)inst->current_cfg.user_ctx; 
        inst->current_cfg.app_rx_cb(node_id, &rx_frame, inst->current_cfg.user_ctx);
    }
    return false;
}

esp_err_t twai_mgr_init_node(twai_mgr_inst_t *inst, const twai_mgr_config_t *cfg) {
    if (!inst || !cfg) return ESP_ERR_INVALID_ARG;

    inst->current_cfg = *cfg;
    inst->bad_state_streak = 0;
    inst->recovery_in_progress = false;

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = { .tx = cfg->tx_io, .rx = cfg->rx_io, .quanta_clk_out = -1, .bus_off_indicator = -1 },
        .bit_timing = { .bitrate = cfg->arb_bitrate },
        .data_timing = { .bitrate = (cfg->mode == TWAI_BUS_MODE_FD) ? cfg->data_bitrate : 0 },
        .timestamp_resolution_hz = 1000000, 
        .fail_retry_cnt = 2,
        .flags = { .enable_self_test = 0, .enable_loopback = 0 },
        .tx_queue_depth = cfg->tx_queue_depth,
    };

    esp_err_t err = twai_new_node_onchip(&node_cfg, &inst->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create node: %s", esp_err_to_name(err));
        return err;
    }

    twai_event_callbacks_t cbs = { 
        .on_rx_done = twai_internal_rx_cb, 
        .on_tx_done = NULL 
    };
    
    err = twai_node_register_event_callbacks(inst->handle, &cbs, inst);
    if (err != ESP_OK) return err;

    return twai_node_enable(inst->handle);
}


esp_err_t twai_mgr_set_filter(twai_mgr_inst_t *inst, uint32_t id1, uint32_t mask1, uint32_t id2, uint32_t mask2, bool is_ext) {
    if (!inst || !inst->handle) return ESP_ERR_INVALID_STATE;

    twai_mask_filter_config_t filter_cfg = twai_make_dual_filter(id1, mask1, id2, mask2, is_ext);
    
    esp_err_t err = twai_node_config_mask_filter(inst->handle, 0, &filter_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set acceptance filter: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Acceptance filter configured successfully");
    return ESP_OK;
}

esp_err_t twai_mgr_async_transmit(twai_mgr_inst_t *inst, const twai_frame_t *tx_frame, TickType_t timeout_ticks) {
    if (!inst || !inst->handle) return ESP_ERR_INVALID_STATE;
    if (inst->recovery_in_progress) return ESP_ERR_INVALID_STATE;

    return twai_node_transmit(inst->handle, tx_frame, timeout_ticks);
}

esp_err_t twai_mgr_reconfigure(twai_mgr_inst_t *inst, const twai_mgr_config_t *new_cfg) {
    if (!inst || !inst->handle) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Reconfiguring CAN node parameters...");
    twai_node_disable(inst->handle);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    twai_node_delete(inst->handle);
    inst->handle = NULL;

    return twai_mgr_init_node(inst, new_cfg);
}

void twai_mgr_health_monitor(twai_mgr_inst_t *inst) {
    if (!inst || !inst->handle) return;

    twai_node_status_t st = {0};
    twai_node_record_t rec = {0};
    
    if (twai_node_get_info(inst->handle, &st, &rec) == ESP_OK) {
        if (st.state == TWAI_ERROR_PASSIVE || st.state == TWAI_ERROR_BUS_OFF) {
            inst->bad_state_streak++;
        } else {
            inst->bad_state_streak = 0;
        }

        if (inst->bad_state_streak >= TWAI_BAD_STATE_THRESHOLD) {
            ESP_LOGW(TAG, "Node BUS_OFF detected! Recovering...");
            inst->recovery_in_progress = true;
            twai_node_disable(inst->handle);
            vTaskDelay(pdMS_TO_TICKS(TWAI_RECOVERY_GUARD_DELAY_MS));
            twai_node_enable(inst->handle);
            inst->recover_count++;
            inst->bad_state_streak = 0;
            inst->recovery_in_progress = false;
        }
    }
}

esp_err_t twai_mgr_init_custom_node(twai_mgr_inst_t *inst, gpio_num_t tx_io, gpio_num_t rx_io, 
                                   twai_bus_mode_t mode, uint32_t arb_bitrate, uint32_t data_bitrate, 
                                   uint32_t queue_depth, void *user_ctx, twai_mgr_app_rx_cb_t rx_cb) {
    twai_mgr_config_t cfg = {
        .tx_io = tx_io,
        .rx_io = rx_io,
        .mode = mode,
        .arb_bitrate = arb_bitrate,
        .data_bitrate = data_bitrate,
        .tx_queue_depth = queue_depth,
        .user_ctx = user_ctx,
        .app_rx_cb = rx_cb
    };
    return twai_mgr_init_node(inst, &cfg);
}

uint8_t twai_mgr_bound_dlc(uint8_t dlc) { return (dlc > 15) ? 15 : dlc; }

uint8_t twai_mgr_dlc_to_len(uint8_t dlc) {
    if (dlc <= 8) return dlc;
    switch (dlc) {
        case 9: return 12; case 10: return 16; case 11: return 20;
        case 12: return 24; case 13: return 32; case 14: return 48;
        case 15: return 64; default: return 8;
    }
}