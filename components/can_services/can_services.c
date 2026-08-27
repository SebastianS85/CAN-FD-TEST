#include "can_services.h"
#include "can_logger.h"
#include "wifi_manager.h"
#include "app_stats_ui.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "can_services";
static can_services_config_t s_cfg = {0};

static void twai_health_task(void *pvParameters) {
    while (1) {
        if (s_cfg.node1) twai_mgr_health_monitor(s_cfg.node1);
        if (s_cfg.node2) twai_mgr_health_monitor(s_cfg.node2);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void can_routing_task(void *pvParameters) {
    size_t item_size;
    while (1) {
        if (s_cfg.route_ringbuf) {
            void *data = xRingbufferReceive(s_cfg.route_ringbuf, &item_size, portMAX_DELAY);
            if (data != NULL) {
                vRingbufferReturnItem(s_cfg.route_ringbuf, data);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
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

        if (s_cfg.node1 && twai_mgr_async_transmit(s_cfg.node1, &tx_frame_pool[idx], portMAX_DELAY) == ESP_OK) {
            app_stats_inc_gen_tx_ok();
            app_stats_inc_tx_node1();
            frame_counter++;
            
            if (frame_counter % 10000 == 0) {
                ESP_LOGI(TAG, "Sent %lu / %lu frames so far...", (unsigned long)frame_counter, (unsigned long)MAX_FRAMES);
            }
            
            if (frame_counter % 10 == 0) {
                vTaskDelay(1); 
            }
        }
    }
    
    ESP_LOGW(TAG, "====== GENERATED %lu FRAMES! ======", (unsigned long)frame_counter);
    ESP_LOGI(TAG, "Waiting 3 seconds for Ringbufs to empty...");
    
    vTaskDelay(pdMS_TO_TICKS(3000));
    can_logger_request_stop();
    vTaskDelete(NULL);
}

esp_err_t can_services_start(const can_services_config_t *config) {
    if (config) s_cfg = *config;

    if (s_cfg.route_ringbuf) {
        xTaskCreatePinnedToCore(can_routing_task, "can_route", 4096, NULL, 3, NULL, 0);
    }
    xTaskCreatePinnedToCore(twai_health_task, "twai_health", 3072, NULL, 2, NULL, 0); 
    if (s_cfg.enable_generator) {
        xTaskCreatePinnedToCore(osci_tx_task, "osci_tx", 4096, NULL, 3, NULL, 0);
    }
    return ESP_OK;
}
