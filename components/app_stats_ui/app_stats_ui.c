#include <stdio.h>
#include "app_stats_ui.h"
#include "c_oled.h"
#include "wifi_manager.h"
#include "can_logger.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static app_stats_ui_config_t s_cfg = {0};
static app_stats_ui_metrics_t s_metrics = {0};

volatile app_stats_ui_metrics_t *app_stats_ui_get_metrics(void) { return &s_metrics; }

void app_stats_inc_rx_node1(void) { s_metrics.rx_frames_node1++; s_metrics.rx_frames++; }
void app_stats_inc_rx_node2(void) { s_metrics.rx_frames_node2++; s_metrics.rx_frames++; }
void app_stats_inc_tx_node1(void) { s_metrics.tx_frames_node1++; }
void app_stats_inc_tx_node2(void) { s_metrics.tx_frames_node2++; }
void app_stats_inc_drop_sd(void) { s_metrics.log_drops++; }
void app_stats_inc_drop_udp(void) { s_metrics.transport_drops++; }
void app_stats_inc_drop_route(void) { s_metrics.gateway_drops++; }
void app_stats_inc_gen_tx_ok(void) { s_metrics.generator_tx_ok++; }

static void diagnostics_task(void *pvParameters) {
    while (1) {
        if (s_cfg.metrics) {
            uint32_t backlog = s_cfg.metrics->ringbuf_in - s_cfg.metrics->ringbuf_out;

            printf("[SYSTEM] Mode: %lu | RX: %lu | GW Drops: %lu | Backlog: %lu\n",
                   (unsigned long)s_cfg.display_mode,
                   (unsigned long)s_cfg.metrics->rx_frames,
                   (unsigned long)s_cfg.metrics->gateway_drops,
                   (unsigned long)backlog);
        } else {
            printf("[DIAG] N1(TX/RX):%lu/%lu | N2(TX/RX):%lu/%lu | Drops(SD):%lu\n",
                     (unsigned long)s_metrics.tx_frames_node1, (unsigned long)s_metrics.rx_frames_node1,
                     (unsigned long)s_metrics.tx_frames_node2, (unsigned long)s_metrics.rx_frames_node2,
                     (unsigned long)s_metrics.log_drops);
        }
               
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void oled_display_task(void *arg) {
    char text_buf[48];

    while (1) {
        uint8_t port_val = 0;
        if (pcf8574_read_byte(s_cfg.pcf8574, &port_val) == ESP_OK) {
            port_val |= 0x0F;
            port_val |= 0xF0;

            if (s_cfg.display_mode == APP_DISPLAY_MODE_BRIDGE) {
                port_val &= ~(1 << 4);
            } else if (s_cfg.display_mode == APP_DISPLAY_MODE_SD_LOGGER) {
                port_val &= ~(1 << 5);
            } else if (s_cfg.display_mode == APP_DISPLAY_MODE_TCP_SERVER) {
                port_val &= ~(1 << 6);
            }

            pcf8574_write_byte(s_cfg.pcf8574, port_val);
        }

        c_oled_clear_buffer();

        if (can_logger_is_stop_requested()) {
            c_oled_draw_string(0, 0, "SD Card Ejected");
            c_oled_draw_string(0, 2, "Safe to remove!");
            c_oled_draw_string(0, 4, "You can power off");
        } else {
            if (s_cfg.display_mode == APP_DISPLAY_MODE_BRIDGE) {
                c_oled_draw_string(0, 0, "CAN Bridge Mode");
            } else if (s_cfg.display_mode == APP_DISPLAY_MODE_SD_LOGGER) {
                c_oled_draw_string(0, 0, "SD Logger Mode");
            } else {
                c_oled_draw_string(0, 0, "TCP Server Mode");
            }

            snprintf(text_buf, sizeof(text_buf), "C1 T:%lu R:%lu",
                     (unsigned long)s_cfg.metrics->tx_frames_node1,
                     (unsigned long)s_cfg.metrics->rx_frames_node1);
            c_oled_draw_string(0, 2, text_buf);

            snprintf(text_buf, sizeof(text_buf), "C2 T:%lu R:%lu",
                     (unsigned long)s_cfg.metrics->tx_frames_node2,
                     (unsigned long)s_cfg.metrics->rx_frames_node2);
            c_oled_draw_string(0, 4, text_buf);

            if (s_cfg.display_mode == APP_DISPLAY_MODE_TCP_SERVER && wifi_manager_is_connected()) {
                snprintf(text_buf, sizeof(text_buf), "%s", wifi_manager_get_ip());
            } else {
                snprintf(text_buf, sizeof(text_buf), "GW Drops: %lu",
                         (unsigned long)s_cfg.metrics->gateway_drops);
            }
            c_oled_draw_string(0, 6, text_buf);
        }

        c_oled_update();
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }
}

void app_stats_ui_start(const app_stats_ui_config_t *config) {
    if (config) s_cfg = *config;

    xTaskCreatePinnedToCore(diagnostics_task, "diag", 3072, NULL, 2, NULL, 0);
    if (s_cfg.oled_enabled && s_cfg.pcf8574 && s_cfg.metrics) {
        xTaskCreatePinnedToCore(oled_display_task, "oled", 4096, NULL, 2, NULL, 0);
    }
}
