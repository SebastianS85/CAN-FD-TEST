#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "secrets.h"
#include "app_setup.h"
#include "app_peripherals.h"
#include "app_stats_ui.h"
#include "can_services.h"
#include "c_oled.h"

static const app_setup_can_config_t can_config = {
    .node1 = {
        .arbitration_bitrate = 500000,
        .data_bitrate =0,
        .mode = CAN_MODE_NORMAL,
        .listen_only =false,
    },
    .node2 = {
        .arbitration_bitrate = 500000,
        .data_bitrate =0,
        .mode = CAN_MODE_NORMAL,
        .listen_only = false,
    },
};


void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_log_level_set("esp_twai", ESP_LOG_WARN);
    ESP_ERROR_CHECK(app_setup_init(WIFI_SSID, WIFI_PASS, &can_config));
   // ESP_ERROR_CHECK(c_oled_set_rotation_180(false));
   
}