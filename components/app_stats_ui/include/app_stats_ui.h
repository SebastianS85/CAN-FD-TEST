#ifndef APP_STATS_UI_H
#define APP_STATS_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "twai_manager.h"

typedef struct {
    bool oled_enabled;
    twai_mgr_inst_t *node1;
    twai_mgr_inst_t *node2;
} app_stats_ui_config_t;

void app_stats_ui_start(const app_stats_ui_config_t *config);

void app_stats_inc_rx_node1(void);
void app_stats_inc_rx_node2(void);
void app_stats_inc_tx_node1(void);
void app_stats_inc_tx_node2(void);
void app_stats_inc_drop_sd(void);
void app_stats_inc_drop_udp(void);
void app_stats_inc_drop_route(void);
void app_stats_inc_gen_tx_ok(void);

#endif 
