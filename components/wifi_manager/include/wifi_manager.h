#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *ssid;
    const char *password;
} wifi_manager_config_t;

esp_err_t wifi_manager_init(const wifi_manager_config_t *config);
bool wifi_manager_is_connected(void);
const char* wifi_manager_get_ip(void);

#endif // WIFI_MANAGER_H
