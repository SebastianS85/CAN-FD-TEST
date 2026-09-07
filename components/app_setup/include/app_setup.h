#ifndef APP_SETUP_H
#define APP_SETUP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
	CAN_MODE_NORMAL,
	CAN_MODE_FD,
} app_setup_can_mode_t;

typedef struct {
	uint32_t arbitration_bitrate;
	uint32_t data_bitrate;
	app_setup_can_mode_t mode;
	bool listen_only;
} app_setup_can_node_config_t;

typedef struct {
	app_setup_can_node_config_t node1;
	app_setup_can_node_config_t node2;
} app_setup_can_config_t;

esp_err_t app_setup_init(const char *wifi_ssid, const char *wifi_password,
						 const app_setup_can_config_t *can_config);

#endif