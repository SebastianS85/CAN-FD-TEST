#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <arpa/inet.h>
#include "esp_heap_caps.h"
#include "secrets.h"

#include "twai_manager.h"
#include "app_buffers.h"
#include "app_peripherals.h"
#include "app_types.h"
#include "app_stats_ui.h"
#include "can_logger.h"
#include "can_services.h"
#include "tcp_service.h"
#include "wifi_manager.h"
#include "app_config.h"

void app_main(void)
{



    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_flash_init();
    esp_log_level_set("esp_twai", ESP_LOG_WARN);

    const app_buffers_config_t buffer_config = {
        .log_buf_size = CAN_LOG_RINGBUF_SIZE,
        .route_buf_size = CAN_ROUTE_RINGBUF_SIZE,
    };
    app_buffers_t buffers = {0};
    ESP_ERROR_CHECK(app_buffers_init(&buffer_config, &buffers));
    app_display_mode_t app_mode = APP_DISPLAY_MODE_BRIDGE;
    volatile app_stats_ui_metrics_t *metrics = app_stats_ui_get_metrics();

    const app_peripherals_config_t peripherals_config = {
        .port = APP_I2C_PORT,
        .sda_pin = APP_I2C_SDA_PIN,
        .scl_pin = APP_I2C_SCL_PIN,
        .freq_hz = APP_I2C_FREQUENCY_HZ,
        .oled_addr = OLED_I2C_ADDRESS,
        .pcf8574_addr = PCF8574_I2C_ADDRESS,
    };
    ESP_ERROR_CHECK(app_peripherals_init(&peripherals_config));
    app_peripherals_handles_t *peripherals = app_peripherals_get_handles();
    ESP_ERROR_CHECK(app_peripherals_set_datetime(
    2026,  // year
    9,     // month
    1,     // day of month
    2,     // day of week
    17,    // hour
    56,    // minute
    0      // second
));

    uint8_t dip_state = 0xFF;
    if (pcf8574_read_byte(&peripherals->pcf8574, &dip_state) == ESP_OK)
    {
        if ((dip_state & (1 << 0)) == 0) {
            app_mode = APP_DISPLAY_MODE_BRIDGE;
        } 
        else if ((dip_state & (1 << 1)) == 0) {
            app_mode = APP_DISPLAY_MODE_SD_LOGGER;
        } 
        else if ((dip_state & (1 << 2)) == 0) {
            app_mode = APP_DISPLAY_MODE_TCP_SERVER;
        } 
        else {
            app_mode = APP_DISPLAY_MODE_BRIDGE;
        }
    }

    if (app_mode == APP_DISPLAY_MODE_SD_LOGGER)
    {
        sdcard_service_config_t sd_cfg = {
            .pin_mosi = SD_MOSI_PIN, .pin_miso = SD_MISO_PIN, .pin_sclk = SD_CLK_PIN, .pin_cs = SD_CS_PIN, .host_id = SPI2_HOST, .format_if_mount_failed = true, .max_files = 5, .mount_point = "/sdcard"};
        can_logger_config_t logger_cfg = {
            .eject_button_pin = SD_EJECT_BUTTON_PIN,
            .ringbuf = buffers.log_ringbuf,
            .ringbuf_out_count = &metrics->ringbuf_out,
            .rtc_dev = &peripherals->rtc,
            .sd_hw_config = sd_cfg,
        };
        ESP_ERROR_CHECK(can_logger_init(&logger_cfg));
    }
    else if (app_mode == APP_DISPLAY_MODE_TCP_SERVER)
    {
        const wifi_manager_config_t wifi_config = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        };
        ESP_ERROR_CHECK(wifi_manager_init(&wifi_config));
    }

    const can_services_config_t can_services_config = {
        .node1_config = {
            .tx_pin = CAN1_TX_PIN,
            .rx_pin = CAN1_RX_PIN,
            .mode = TWAI_BUS_MODE_FD,
            .arbitration_bitrate = CAN1_BITRATE,
            .data_bitrate = CAN1_DATA_BITRATE,
            .tx_queue_depth = CAN_TX_QUEUE_DEPTH,
        },
        .node2_config = {
            .tx_pin = CAN2_TX_PIN,
            .rx_pin = CAN2_RX_PIN,
            .mode = TWAI_BUS_MODE_FD,
            .arbitration_bitrate = CAN2_BITRATE,
            .data_bitrate = CAN2_DATA_BITRATE,
            .tx_queue_depth = CAN_TX_QUEUE_DEPTH,
        },
        .bridge_mode = app_mode == APP_DISPLAY_MODE_BRIDGE,
        .log_ringbuf = buffers.log_ringbuf,
        .route_ringbuf = app_mode == APP_DISPLAY_MODE_BRIDGE ? buffers.route_ringbuf : NULL,
        .route_use_fd_frames = APP_USE_CAN_FD,
        .route_tx_count = &metrics->tx_frames_node2,
        .log_drop_count = &metrics->log_drops,
        .route_drop_count = &metrics->gateway_drops,
        .rx_count = &metrics->rx_frames,
        .rx_count_node1 = &metrics->rx_frames_node1,
        .rx_count_node2 = &metrics->rx_frames_node2,
        .log_ringbuf_in_count = &metrics->ringbuf_in,
    };
    ESP_ERROR_CHECK(can_services_configure(&can_services_config));
    ESP_ERROR_CHECK(can_services_init_nodes());
    ESP_ERROR_CHECK(can_services_start(NULL));

    const app_stats_ui_config_t stats_ui_config = {
        .oled_enabled = peripherals->oled_available,
        .pcf8574 = &peripherals->pcf8574,
        .display_mode = app_mode,
        .metrics = metrics,
    };
    app_stats_ui_start(&stats_ui_config);

    if (app_mode == APP_DISPLAY_MODE_TCP_SERVER)
    {
        const tcp_service_config_t tcp_config = {
            .tx_port = TCP_TX_PORT,
            .rx_port = TCP_RX_PORT,
            .ringbuf = buffers.log_ringbuf,
            .node1 = can_services_get_node(1),
            .node2 = can_services_get_node(2),
            .use_fd_frames = APP_USE_CAN_FD,
            .tx_frames_node1 = &metrics->tx_frames_node1,
            .tx_frames_node2 = &metrics->tx_frames_node2,
            .ringbuf_out = &metrics->ringbuf_out,
        };
        ESP_ERROR_CHECK(tcp_service_start(&tcp_config));
    }

}