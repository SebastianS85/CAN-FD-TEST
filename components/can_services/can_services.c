#include "can_services.h"
#include "can_logger.h"
#include "wifi_manager.h"
#include "app_stats_ui.h"
#include "app_mode_state.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "can_services";
static can_services_config_t s_cfg = {0};
static twai_mgr_inst_t s_node1 = {0};
static twai_mgr_inst_t s_node2 = {0};

static bool can_services_bitrate_is_valid(uint32_t bitrate)
{
    switch (bitrate) {
        case 125000:
        case 250000:
        case 500000:
        case 800000:
        case 1000000:
        case 2000000:
        case 4000000:
        case 5000000:
            return true;
        default:
            return false;
    }
}

static void can_services_load_saved_bitrates(void)
{
    nvs_handle_t handle;
    if (nvs_open("can_settings", NVS_READWRITE, &handle) != ESP_OK) return;

    uint32_t bitrate;
    if (nvs_get_u32(handle, "can1_arb", &bitrate) == ESP_OK && can_services_bitrate_is_valid(bitrate)) {
        s_cfg.node1_config.arbitration_bitrate = bitrate;
    }
    if (nvs_get_u32(handle, "can1_data", &bitrate) == ESP_OK &&
        (bitrate == 0 || can_services_bitrate_is_valid(bitrate))) {
        s_cfg.node1_config.data_bitrate = bitrate;
    }
    if (nvs_get_u32(handle, "can2_arb", &bitrate) == ESP_OK && can_services_bitrate_is_valid(bitrate)) {
        s_cfg.node2_config.arbitration_bitrate = bitrate;
    }
    if (nvs_get_u32(handle, "can2_data", &bitrate) == ESP_OK &&
        (bitrate == 0 || can_services_bitrate_is_valid(bitrate))) {
        s_cfg.node2_config.data_bitrate = bitrate;
    }
    s_cfg.node1_config.mode = s_cfg.node1_config.data_bitrate == 0 ?
                              TWAI_BUS_MODE_CLASSIC : TWAI_BUS_MODE_FD;
    s_cfg.node2_config.mode = s_cfg.node2_config.data_bitrate == 0 ?
                              TWAI_BUS_MODE_CLASSIC : TWAI_BUS_MODE_FD;
    nvs_close(handle);
}

static esp_err_t can_services_save_bitrates(uint8_t node_id, uint32_t arbitration_bitrate,
                                            uint32_t data_bitrate)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("can_settings", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    const char *arb_key = node_id == 1 ? "can1_arb" : "can2_arb";
    const char *data_key = node_id == 1 ? "can1_data" : "can2_data";
    err = nvs_set_u32(handle, arb_key, arbitration_bitrate);
    if (err == ESP_OK) err = nvs_set_u32(handle, data_key, data_bitrate);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

void can_services_rx_handler(uint8_t node_id, const twai_frame_t *rx_frame, void *user_ctx) {
    (void)user_ctx;
    if (!rx_frame) return;

    log_frame_t frame = {0};
    frame.timestamp = (uint32_t)(rx_frame->header.timestamp / 1000);
    frame.node_id = node_id;
    frame.id = rx_frame->header.id;
    if (rx_frame->header.ide) {
        frame.id |= 0x80000000U;
    }

    frame.dlc = rx_frame->header.dlc;
    uint8_t copy_len = twai_mgr_dlc_to_len(rx_frame->header.dlc);
    if (copy_len > sizeof(frame.data)) copy_len = sizeof(frame.data);

    if (rx_frame->buffer && copy_len > 0) {
        uint8_t actual_len = rx_frame->buffer_len < copy_len ? rx_frame->buffer_len : copy_len;
        memcpy(frame.data, rx_frame->buffer, actual_len);
    }

    BaseType_t awoken = pdFALSE;
    app_display_mode_t mode = app_mode_state_get_mode();
    if (mode == APP_DISPLAY_MODE_BRIDGE) {
        if (node_id == 1 && s_cfg.route_ringbuf &&
            xRingbufferSendFromISR(s_cfg.route_ringbuf, &frame, sizeof(frame), &awoken) != pdTRUE) {
            if (s_cfg.route_drop_count) (*s_cfg.route_drop_count)++;
        }
    } else if ((mode == APP_DISPLAY_MODE_SD_LOGGER || mode == APP_DISPLAY_MODE_TCP_SERVER) && s_cfg.log_ringbuf) {
        if (xRingbufferSendFromISR(s_cfg.log_ringbuf, &frame, sizeof(frame), &awoken) != pdTRUE) {
            if (s_cfg.log_drop_count) (*s_cfg.log_drop_count)++;
        } else if (s_cfg.log_ringbuf_in_count) {
            (*s_cfg.log_ringbuf_in_count)++;
        }
    }

    if (node_id == 1 && s_cfg.rx_count_node1) (*s_cfg.rx_count_node1)++;
    if (node_id == 2 && s_cfg.rx_count_node2) (*s_cfg.rx_count_node2)++;
    if (s_cfg.rx_count) (*s_cfg.rx_count)++;

    if (awoken == pdTRUE) portYIELD_FROM_ISR();
}

static void twai_health_task(void *pvParameters) {
    bool node1_was_recovering = false;
    bool node2_was_recovering = false;
    uint32_t node1_last_queue_full_count = 0;
    uint32_t node2_last_queue_full_count = 0;

    while (1) {
        twai_mgr_health_monitor(&s_node1);
        twai_mgr_health_monitor(&s_node2);

        if (s_node1.recovery_in_progress && !node1_was_recovering) {
                ESP_LOGE(TAG, "FAULT: CAN Bus 1");
        } else if (!s_node1.recovery_in_progress && node1_was_recovering) {
                ESP_LOGI(TAG, "SUCCESS: CAN Bus 1");
        }
        node1_was_recovering = s_node1.recovery_in_progress;

        if (s_node2.recovery_in_progress && !node2_was_recovering) {
                ESP_LOGE(TAG, "FAULT: CAN Bus 2");
        } else if (!s_node2.recovery_in_progress && node2_was_recovering) {
                ESP_LOGI(TAG, "SUCCESS: CAN Bus 2");
        }
        node2_was_recovering = s_node2.recovery_in_progress;

        if (s_node1.tx_queue_full_count != node1_last_queue_full_count) {
            ESP_LOGW(TAG, "CAN Bus 1 TX queue full: %lu events",
                     (unsigned long)s_node1.tx_queue_full_count);
            node1_last_queue_full_count = s_node1.tx_queue_full_count;
        }
        if (s_node2.tx_queue_full_count != node2_last_queue_full_count) {
            ESP_LOGW(TAG, "CAN Bus 2 TX queue full: %lu events",
                     (unsigned long)s_node2.tx_queue_full_count);
            node2_last_queue_full_count = s_node2.tx_queue_full_count;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void can_routing_task(void *pvParameters) {
    size_t item_size;
    while (1) {
        void *data = xRingbufferReceive(s_cfg.route_ringbuf, &item_size, portMAX_DELAY);
        if (data != NULL) {
            log_frame_t route_frame;
            memcpy(&route_frame, data, sizeof(route_frame));
            vRingbufferReturnItem(s_cfg.route_ringbuf, data);

            if (!s_cfg.route_target->recovery_in_progress) {
                uint8_t safe_dlc = twai_mgr_bound_dlc(route_frame.dlc);
                uint8_t payload_len = twai_mgr_dlc_to_len(safe_dlc);
                if (!s_cfg.route_use_fd_frames && payload_len > 8) {
                    payload_len = 8;
                }

                twai_frame_t tx_frame = {
                    .header = {
                        .id = route_frame.id & 0x1FFFFFFFU,
                        .ide = (route_frame.id & 0x80000000U) ? 1 : 0,
                        .fdf = s_cfg.route_use_fd_frames,
                        .brs = s_cfg.route_use_fd_frames ? 1 : 0,
                        .esi = 0,
                        .dlc = s_cfg.route_use_fd_frames ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc),
                    },
                    .buffer = route_frame.data,
                    .buffer_len = payload_len,
                };

                if (twai_mgr_async_transmit(s_cfg.route_target, &tx_frame, 0) == ESP_OK &&
                    s_cfg.route_tx_count) {
                    (*s_cfg.route_tx_count)++;
                }
            }
        }
    }
}

static void osci_tx_task(void *pvParameters) {
    while (!wifi_manager_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "CAN Generator START: Target 100,000 frames!");
    
    static twai_frame_t tx_frame_pool[128];
    static uint8_t tx_payload_pool[128][64]; 

    uint32_t frame_counter = 0;
    const uint32_t MAX_FRAMES = 100000;

    while (frame_counter < MAX_FRAMES) {
        if (!s_cfg.enable_generator || can_logger_is_stop_requested()) { 
            vTaskDelay(pdMS_TO_TICKS(100)); 
            continue; 
        }

        uint32_t idx = frame_counter % 128;

        memset(tx_payload_pool[idx], 0, 64);
        memcpy(tx_payload_pool[idx], &frame_counter, sizeof(uint32_t));

        tx_frame_pool[idx].header.id = 0x55;
        tx_frame_pool[idx].header.ide = 0;
        tx_frame_pool[idx].header.fdf = 1;
        tx_frame_pool[idx].header.brs = 1;
        tx_frame_pool[idx].header.esi = 0;
        tx_frame_pool[idx].header.dlc = 15;
        tx_frame_pool[idx].buffer = tx_payload_pool[idx];
        tx_frame_pool[idx].buffer_len = 64;

        if (twai_mgr_async_transmit(&s_node1, &tx_frame_pool[idx], pdMS_TO_TICKS(20)) == ESP_OK) {
            app_stats_inc_gen_tx_ok();
            app_stats_inc_tx_node1();
            frame_counter++;
            
            if (frame_counter % 10000 == 0) {
                ESP_LOGI(TAG, "Sent %lu / %lu frames so far...", (unsigned long)frame_counter, (unsigned long)MAX_FRAMES);
            }
            
            if (frame_counter % 10 == 0) {
                vTaskDelay(1); 
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    ESP_LOGW(TAG, "====== GENERATED %lu FRAMES! ======", (unsigned long)frame_counter);
    ESP_LOGI(TAG, "Waiting 3 seconds for Ringbufs to empty...");
    
    vTaskDelay(pdMS_TO_TICKS(3000));
    can_logger_request_stop();
    vTaskDelete(NULL);
}

esp_err_t can_services_configure(const can_services_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    s_cfg = *config;
    can_services_load_saved_bitrates();
    /* Routing capability follows whether a route ring buffer was wired up, not a
     * fixed boot mode, so remote mode can enable bridging at runtime. */
    s_cfg.route_target = s_cfg.route_ringbuf ? &s_node2 : NULL;
    return ESP_OK;
}

esp_err_t can_services_init_nodes(void) {
    esp_err_t err = twai_mgr_init_custom_node(
        &s_node1,
        s_cfg.node1_config.tx_pin,
        s_cfg.node1_config.rx_pin,
        s_cfg.node1_config.mode,
        s_cfg.node1_config.arbitration_bitrate,
        s_cfg.node1_config.data_bitrate,
        s_cfg.node1_config.tx_queue_depth,
        s_cfg.node1_config.listen_only,
        (void *)1,
        can_services_rx_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize CAN node 1: %s", esp_err_to_name(err));
        return err;
    }

    return twai_mgr_init_custom_node(
        &s_node2,
        s_cfg.node2_config.tx_pin,
        s_cfg.node2_config.rx_pin,
        s_cfg.node2_config.mode,
        s_cfg.node2_config.arbitration_bitrate,
        s_cfg.node2_config.data_bitrate,
        s_cfg.node2_config.tx_queue_depth,
        s_cfg.node2_config.listen_only,
        (void *)2,
        can_services_rx_handler);
}

twai_mgr_inst_t *can_services_get_node(uint8_t node_id) {
    if (node_id == 1) return &s_node1;
    if (node_id == 2) return &s_node2;
    return NULL;
}

esp_err_t can_services_reconfigure_node(uint8_t node_id, uint32_t arbitration_bitrate,
                                        uint32_t data_bitrate, bool listen_only)
{
    if ((node_id != 1 && node_id != 2) || !can_services_bitrate_is_valid(arbitration_bitrate) ||
        (data_bitrate != 0 && !can_services_bitrate_is_valid(data_bitrate))) {
        return ESP_ERR_INVALID_ARG;
    }

    can_services_node_config_t *node_cfg = node_id == 1 ? &s_cfg.node1_config : &s_cfg.node2_config;
    twai_mgr_inst_t *node = node_id == 1 ? &s_node1 : &s_node2;
    twai_mgr_config_t new_cfg = node->current_cfg;
    new_cfg.arb_bitrate = arbitration_bitrate;
    new_cfg.data_bitrate = data_bitrate;
    new_cfg.mode = data_bitrate == 0 ? TWAI_BUS_MODE_CLASSIC : TWAI_BUS_MODE_FD;
    new_cfg.listen_only = listen_only;

    ESP_LOGI(TAG, "Stopping CAN %u before bitrate update", node_id);
    esp_err_t err = twai_mgr_reconfigure(node, &new_cfg);
    if (err != ESP_OK) return err;

    node_cfg->arbitration_bitrate = arbitration_bitrate;
    node_cfg->data_bitrate = data_bitrate;
    node_cfg->mode = new_cfg.mode;
    node_cfg->listen_only = listen_only;
    err = can_services_save_bitrates(node_id, arbitration_bitrate, data_bitrate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN %u updated but could not be saved: %s", node_id, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CAN %u restarted at %lu/%lu bit/s", node_id,
                 (unsigned long)arbitration_bitrate, (unsigned long)data_bitrate);
    }
    return err;
}

esp_err_t can_services_start(const can_services_config_t *config) {
    if (config) {
        esp_err_t err = can_services_configure(config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Invalid CAN services configuration: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (s_cfg.route_ringbuf && s_cfg.route_target) {
        xTaskCreatePinnedToCore(can_routing_task, "can_route", 4096, NULL, 3, NULL, 0);
    }
    xTaskCreatePinnedToCore(twai_health_task, "twai_health", 3072, NULL, 2, NULL, 0); 
    if (s_cfg.enable_generator) {
        xTaskCreatePinnedToCore(osci_tx_task, "osci_tx", 4096, NULL, 3, NULL, 0);
    }
    return ESP_OK;
}
