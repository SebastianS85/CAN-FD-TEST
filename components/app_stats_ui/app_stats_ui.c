#include <stdio.h>
#include "app_stats_ui.h"
#include "c_oled.h"
#include "wifi_manager.h"
#include "can_logger.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile uint32_t g_rx_frames = 0;
static volatile uint32_t g_rx_frames_node1 = 0;
static volatile uint32_t g_rx_frames_node2 = 0;
static volatile uint32_t g_tx_frames_node1 = 0;
static volatile uint32_t g_tx_frames_node2 = 0;

static volatile uint32_t g_drop_sd = 0;
static volatile uint32_t g_drop_udp = 0;
static volatile uint32_t g_drop_route = 0;
static volatile uint32_t g_gen_tx_ok = 0;

static volatile uint32_t g_rx_fps = 0;
static volatile uint32_t g_rx_fps_node1 = 0;
static volatile uint32_t g_rx_fps_node2 = 0;
static volatile uint32_t g_tx_fps = 0;

static app_stats_ui_config_t s_cfg = {0};

void app_stats_inc_rx_node1(void) { g_rx_frames_node1++; g_rx_frames++; }
void app_stats_inc_rx_node2(void) { g_rx_frames_node2++; g_rx_frames++; }
void app_stats_inc_tx_node1(void) { g_tx_frames_node1++; }
void app_stats_inc_tx_node2(void) { g_tx_frames_node2++; }
void app_stats_inc_drop_sd(void) { g_drop_sd++; }
void app_stats_inc_drop_udp(void) { g_drop_udp++; }
void app_stats_inc_drop_route(void) { g_drop_route++; }
void app_stats_inc_gen_tx_ok(void) { g_gen_tx_ok++; }

static void stats_calc_task(void *arg) {
    uint32_t last_rx1 = 0, last_rx2 = 0, last_tx = 0;
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        g_rx_fps_node1 = g_rx_frames_node1 - last_rx1;
        g_rx_fps_node2 = g_rx_frames_node2 - last_rx2;
        g_rx_fps = (g_rx_frames_node1 + g_rx_frames_node2) - (last_rx1 + last_rx2);
        
        g_tx_fps = g_gen_tx_ok - last_tx;
        
        last_rx1 = g_rx_frames_node1; 
        last_rx2 = g_rx_frames_node2; 
        last_tx = g_gen_tx_ok;
    }
}

static void diagnostics_task(void *pvParameters) {
    while (1) {
        twai_node_status_t st1 = {0}; twai_node_record_t rec1 = {0};
        twai_node_status_t st2 = {0}; twai_node_record_t rec2 = {0};
        if (s_cfg.node1 && s_cfg.node1->handle) twai_node_get_info(s_cfg.node1->handle, &st1, &rec1);
        if (s_cfg.node2 && s_cfg.node2->handle) twai_node_get_info(s_cfg.node2->handle, &st2, &rec2);

        printf("[DIAG] N1(TX/RX):%lu/%lu | N2(TX/RX):%lu/%lu | Drops(SD):%lu\n",
               (unsigned long)g_tx_frames_node1, (unsigned long)g_rx_frames_node1,
               (unsigned long)g_tx_frames_node2, (unsigned long)g_rx_frames_node2,
               (unsigned long)g_drop_sd);
               
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void oled_display_task(void *arg) {
    char text_buf[48];

    while (1) {
        c_oled_clear_buffer();

        if (wifi_manager_is_connected()) {
            snprintf(text_buf, sizeof(text_buf), "%s", wifi_manager_get_ip());
        } else {
            snprintf(text_buf, sizeof(text_buf), "Scanning WiFi...");
        }
        c_oled_draw_string(0, 0, text_buf);

        snprintf(text_buf, sizeof(text_buf), "C1 T:%lu R:%lu", (unsigned long)g_tx_frames_node1, (unsigned long)g_rx_frames_node1);
        c_oled_draw_string(0, 2, text_buf);

        snprintf(text_buf, sizeof(text_buf), "C2 T:%lu R:%lu", (unsigned long)g_tx_frames_node2, (unsigned long)g_rx_frames_node2);
        c_oled_draw_string(0, 4, text_buf);

        if (can_logger_is_stop_requested()) {
            snprintf(text_buf, sizeof(text_buf), "SD: SAFE TO EJECT");
        } else {
            snprintf(text_buf, sizeof(text_buf), "DropSD:%lu U:%lu", (unsigned long)g_drop_sd, (unsigned long)g_drop_udp);
        }
        c_oled_draw_string(0, 6, text_buf);

        c_oled_update();
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }
}

void app_stats_ui_start(const app_stats_ui_config_t *config) {
    if (config) s_cfg = *config;

    xTaskCreatePinnedToCore(diagnostics_task, "diag", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(stats_calc_task, "stats_task", 2048, NULL, 2, NULL, 0);
    if (s_cfg.oled_enabled) {
        xTaskCreatePinnedToCore(oled_display_task, "oled", 4096, NULL, 2, NULL, 0);
    }
}
