#include "twai_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "twai_mgr";
#define TWAI_BAD_STATE_THRESHOLD 3
#define TWAI_RECOVERY_GUARD_DELAY_MS 20

typedef struct {
    twai_frame_t frame;
    uint8_t payload[64];
    bool in_use;
} twai_mgr_tx_slot_t;

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

static IRAM_ATTR bool twai_internal_tx_done_cb(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user_ctx) {
    twai_mgr_inst_t *inst = (twai_mgr_inst_t *)user_ctx;
    if (!inst || !edata || !edata->done_tx_frame || !inst->tx_slots) return false;

    twai_mgr_tx_slot_t *slots = (twai_mgr_tx_slot_t *)inst->tx_slots;
    portENTER_CRITICAL_ISR(&inst->tx_slot_lock);
    for (uint16_t index = 0; index < inst->tx_slot_count; index++) {
        if (&slots[index].frame == edata->done_tx_frame) {
            slots[index].in_use = false;
            break;
        }
    }
    portEXIT_CRITICAL_ISR(&inst->tx_slot_lock);
    return false;
}

esp_err_t twai_mgr_init_node(twai_mgr_inst_t *inst, const twai_mgr_config_t *cfg) {
    if (!inst || !cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->tx_queue_depth == 0) return ESP_ERR_INVALID_ARG;
    if (!inst->operation_lock) {
        inst->operation_lock = xSemaphoreCreateMutex();
        if (!inst->operation_lock) return ESP_ERR_NO_MEM;
    }

    inst->current_cfg = *cfg;
    inst->bad_state_streak = 0;
    inst->tx_queue_full_count = 0;
    inst->tx_error_count = 0;
    inst->tx_slot_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    inst->recovery_in_progress = false;
    inst->tx_slot_count = cfg->tx_queue_depth;
    inst->tx_slots = heap_caps_calloc(inst->tx_slot_count, sizeof(twai_mgr_tx_slot_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!inst->tx_slots) {
        inst->tx_slot_count = 0;
        return ESP_ERR_NO_MEM;
    }

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = { .tx = cfg->tx_io, .rx = cfg->rx_io, .quanta_clk_out = -1, .bus_off_indicator = -1 },
        .bit_timing = { .bitrate = cfg->arb_bitrate },
        .data_timing = { .bitrate = (cfg->mode == TWAI_BUS_MODE_FD) ? cfg->data_bitrate : 0 },
        .timestamp_resolution_hz = 1000000, 
        .fail_retry_cnt = 2,
        .flags = { .enable_self_test = 0, .enable_loopback = 0, .enable_listen_only = cfg->listen_only },
        .tx_queue_depth = cfg->tx_queue_depth,
    };

    esp_err_t err = twai_new_node_onchip(&node_cfg, &inst->handle);
    if (err != ESP_OK) {
        heap_caps_free(inst->tx_slots);
        inst->tx_slots = NULL;
        inst->tx_slot_count = 0;
        ESP_LOGE(TAG, "Failed to create node: %s", esp_err_to_name(err));
        return err;
    }

    twai_event_callbacks_t cbs = { 
        .on_rx_done = twai_internal_rx_cb, 
        .on_tx_done = twai_internal_tx_done_cb,
    };
    
    err = twai_node_register_event_callbacks(inst->handle, &cbs, inst);
    if (err != ESP_OK) {
        twai_node_delete(inst->handle);
        inst->handle = NULL;
        heap_caps_free(inst->tx_slots);
        inst->tx_slots = NULL;
        inst->tx_slot_count = 0;
        return err;
    }

    err = twai_node_enable(inst->handle);
    if (err != ESP_OK) {
        twai_node_delete(inst->handle);
        inst->handle = NULL;
        heap_caps_free(inst->tx_slots);
        inst->tx_slots = NULL;
        inst->tx_slot_count = 0;
    }
    return err;
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
    if (!tx_frame || (tx_frame->buffer_len > 0 && !tx_frame->buffer) ||
        tx_frame->buffer_len > sizeof(((twai_mgr_tx_slot_t *)0)->payload)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(inst->operation_lock, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (!inst->handle || inst->recovery_in_progress) {
        xSemaphoreGive(inst->operation_lock);
        return ESP_ERR_INVALID_STATE;
    }

    twai_mgr_tx_slot_t *slots = (twai_mgr_tx_slot_t *)inst->tx_slots;
    twai_mgr_tx_slot_t *slot = NULL;
    portENTER_CRITICAL(&inst->tx_slot_lock);
    for (uint16_t index = 0; index < inst->tx_slot_count; index++) {
        if (!slots[index].in_use) {
            slots[index].in_use = true;
            slot = &slots[index];
            break;
        }
    }
    portEXIT_CRITICAL(&inst->tx_slot_lock);

    if (!slot) {
        inst->tx_queue_full_count++;
        xSemaphoreGive(inst->operation_lock);
        return ESP_ERR_TIMEOUT;
    }

    slot->frame = *tx_frame;
    if (tx_frame->buffer_len > 0) {
        memcpy(slot->payload, tx_frame->buffer, tx_frame->buffer_len);
    }
    slot->frame.buffer = slot->payload;

    esp_err_t err = twai_node_transmit(inst->handle, &slot->frame, timeout_ticks);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&inst->tx_slot_lock);
        slot->in_use = false;
        portEXIT_CRITICAL(&inst->tx_slot_lock);
    }
    if (err == ESP_ERR_TIMEOUT) {
        inst->tx_queue_full_count++;
    } else if (err != ESP_OK) {
        inst->tx_error_count++;
    }
    xSemaphoreGive(inst->operation_lock);
    return err;
}

esp_err_t twai_mgr_reconfigure(twai_mgr_inst_t *inst, const twai_mgr_config_t *new_cfg) {
    if (!inst || !inst->handle) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(inst->operation_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Reconfiguring CAN node parameters...");
    twai_mgr_config_t previous_cfg = inst->current_cfg;
    inst->recovery_in_progress = true;
    esp_err_t err = twai_node_disable(inst->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop CAN node: %s", esp_err_to_name(err));
        inst->recovery_in_progress = false;
        xSemaphoreGive(inst->operation_lock);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    err = twai_node_delete(inst->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete stopped CAN node: %s", esp_err_to_name(err));
        twai_node_enable(inst->handle);
        inst->recovery_in_progress = false;
        xSemaphoreGive(inst->operation_lock);
        return err;
    }
    inst->handle = NULL;
    heap_caps_free(inst->tx_slots);
    inst->tx_slots = NULL;
    inst->tx_slot_count = 0;

    esp_err_t reconfigure_err = twai_mgr_init_node(inst, new_cfg);
    if (reconfigure_err != ESP_OK) {
        ESP_LOGE(TAG, "New CAN configuration failed; restoring previous configuration");
        esp_err_t restore_err = twai_mgr_init_node(inst, &previous_cfg);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Unable to restore previous CAN configuration: %s", esp_err_to_name(restore_err));
        }
    }
    inst->recovery_in_progress = false;
    xSemaphoreGive(inst->operation_lock);
    return reconfigure_err;
}

void twai_mgr_health_monitor(twai_mgr_inst_t *inst) {
    if (!inst || !inst->handle) return;
    if (xSemaphoreTake(inst->operation_lock, 0) != pdTRUE) return;

    twai_node_status_t st = {0};
    twai_node_record_t rec = {0};
    
    if (twai_node_get_info(inst->handle, &st, &rec) == ESP_OK) {
        if (inst->recovery_in_progress) {
            if (st.state != TWAI_ERROR_BUS_OFF) {
                ESP_LOGI(TAG, "CAN node recovered from bus-off");
                inst->recovery_in_progress = false;
            }
        } else if (st.state == TWAI_ERROR_BUS_OFF) {
            inst->bad_state_streak++;
        } else {
            inst->bad_state_streak = 0;
        }

        if (!inst->recovery_in_progress && inst->bad_state_streak >= TWAI_BAD_STATE_THRESHOLD) {
            esp_err_t err = twai_node_recover(inst->handle);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "CAN node bus-off; recovery started");
                inst->recovery_in_progress = true;
                inst->recover_count++;
            } else {
                ESP_LOGE(TAG, "Failed to start CAN bus-off recovery: %s", esp_err_to_name(err));
            }
            inst->bad_state_streak = 0;
        }
    }
    xSemaphoreGive(inst->operation_lock);
}

esp_err_t twai_mgr_init_custom_node(twai_mgr_inst_t *inst, gpio_num_t tx_io, gpio_num_t rx_io, 
                                   twai_bus_mode_t mode, uint32_t arb_bitrate, uint32_t data_bitrate, 
                                   uint32_t queue_depth, bool listen_only, void *user_ctx,
                                   twai_mgr_app_rx_cb_t rx_cb) {
    twai_mgr_config_t cfg = {
        .tx_io = tx_io,
        .rx_io = rx_io,
        .mode = mode,
        .arb_bitrate = arb_bitrate,
        .data_bitrate = data_bitrate,
        .tx_queue_depth = queue_depth,
        .listen_only = listen_only,
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