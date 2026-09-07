#ifndef APP_STATS_UI_H
#define APP_STATS_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "twai_manager.h"
#include "pcf8574.h"
#include "app_types.h"

typedef struct {
    volatile uint32_t rx_frames;
    volatile uint32_t rx_frames_node1;
    volatile uint32_t rx_frames_node2;
    volatile uint32_t tx_frames_node1;
    volatile uint32_t tx_frames_node2;
    volatile uint32_t log_drops;
    volatile uint32_t gateway_drops;
    volatile uint32_t ringbuf_in;
    volatile uint32_t ringbuf_out;
    volatile uint32_t generator_tx_ok;
    volatile uint32_t transport_drops;
} app_stats_ui_metrics_t;

typedef struct {
    bool oled_enabled;
    pcf8574_t *pcf8574;
    volatile app_stats_ui_metrics_t *metrics;
} app_stats_ui_config_t;

void app_stats_ui_start(const app_stats_ui_config_t *config);
volatile app_stats_ui_metrics_t *app_stats_ui_get_metrics(void);

void app_stats_inc_rx_node1(void);
void app_stats_inc_rx_node2(void);
void app_stats_inc_tx_node1(void);
void app_stats_inc_tx_node2(void);
void app_stats_inc_drop_sd(void);
void app_stats_inc_drop_udp(void);
void app_stats_inc_drop_route(void);
void app_stats_inc_gen_tx_ok(void);

#endif 
