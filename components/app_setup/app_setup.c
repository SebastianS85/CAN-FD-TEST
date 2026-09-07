#include "app_setup.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_buffers.h"
#include "app_mode_state.h"
#include "app_peripherals.h"
#include "app_stats_ui.h"
#include "app_types.h"
#include "can_logger.h"
#include "can_services.h"
#include "pcf8574.h"
#include "sdcard_service.h"
#include "tcp_service.h"
#include "wifi_manager.h"

static const char *TAG = "app_setup";

esp_err_t app_setup_init(const char *wifi_ssid, const char *wifi_password,
                         const app_setup_can_config_t *can_config)
{
    if (!can_config) {
        return ESP_ERR_INVALID_ARG;
    }

    const app_buffers_config_t buffer_config = {
        .log_buf_size = CAN_LOG_RINGBUF_SIZE,
        .route_buf_size = CAN_ROUTE_RINGBUF_SIZE,
    };
    app_buffers_t buffers = {0};
    ESP_RETURN_ON_ERROR(app_buffers_init(&buffer_config, &buffers), "app_setup", "initialize buffers");

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
    ESP_RETURN_ON_ERROR(app_peripherals_init(&peripherals_config), "app_setup", "initialize peripherals");
    app_peripherals_handles_t *peripherals = app_peripherals_get_handles();

    uint8_t dip_state = 0xFF;
    bool remote_enabled = false;
    esp_err_t dip_read_err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        dip_read_err = pcf8574_read_byte(&peripherals->pcf8574, &dip_state);
        if (dip_read_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "DIP switch read attempt %d failed: %s", attempt + 1, esp_err_to_name(dip_read_err));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (dip_read_err == ESP_OK) {
        /* DIP4/A4 wired to PCF8574 P0: low enables remote mode selection from
         * the Python app. While remote is enabled, the other two switches are
         * ignored and the active mode is chosen at runtime instead of being
         * fixed at boot. All switches high (no mode selected) also falls back
         * to remote/TCP config mode instead of silently defaulting to bridge. */
        remote_enabled = (dip_state & (1 << 0)) == 0 || (dip_state & 0x0F) == 0x0F;
        if (!remote_enabled) {
            if ((dip_state & (1 << 1)) == 0) {
                app_mode = APP_DISPLAY_MODE_SD_LOGGER;
            } else if ((dip_state & (1 << 2)) == 0) {
                app_mode = APP_DISPLAY_MODE_TCP_SERVER;
            }
        } else {
            /* Remote mode starts idle; the client must explicitly pick a mode. */
            app_mode = APP_DISPLAY_MODE_NONE;
        }
        ESP_LOGI(TAG, "DIP switch raw byte: 0x%02X -> remote_enabled=%d, boot_mode=%d",
                 dip_state, remote_enabled, app_mode);
    } else {
        ESP_LOGE(TAG, "DIP switch read failed after retries; defaulting to CAN bridge mode");
    }
    app_mode_state_init(remote_enabled, app_mode);

    const bool tcp_needed = remote_enabled || app_mode == APP_DISPLAY_MODE_TCP_SERVER;
    const bool sd_needed = remote_enabled || app_mode == APP_DISPLAY_MODE_SD_LOGGER;
    const bool bridge_capable = remote_enabled || app_mode == APP_DISPLAY_MODE_BRIDGE;

    if (sd_needed) {
        const sdcard_service_config_t sd_cfg = {
            .pin_mosi = SD_MOSI_PIN, .pin_miso = SD_MISO_PIN, .pin_sclk = SD_CLK_PIN,
            .pin_cs = SD_CS_PIN, .host_id = SPI2_HOST, .format_if_mount_failed = true,
            .max_files = 5, .mount_point = "/sdcard",
        };
        const can_logger_config_t logger_cfg = {
            .eject_button_pin = SD_EJECT_BUTTON_PIN,
            .ringbuf = buffers.log_ringbuf,
            .ringbuf_out_count = &metrics->ringbuf_out,
            .rtc_dev = &peripherals->rtc,
            .sd_hw_config = sd_cfg,
        };
        ESP_RETURN_ON_ERROR(can_logger_init(&logger_cfg), "app_setup", "initialize CAN logger");
    }

    if (tcp_needed) {
        const wifi_manager_config_t wifi_config = {
            .ssid = wifi_ssid,
            .password = wifi_password,
        };
        ESP_RETURN_ON_ERROR(wifi_manager_init(&wifi_config), "app_setup", "initialize Wi-Fi");
    }

    const can_services_config_t can_services_config = {
        .node1_config = {.tx_pin = CAN1_TX_PIN, .rx_pin = CAN1_RX_PIN,
                         .mode = can_config->node1.mode == CAN_MODE_FD ? TWAI_BUS_MODE_FD : TWAI_BUS_MODE_CLASSIC,
                         .arbitration_bitrate = can_config->node1.arbitration_bitrate,
                         .data_bitrate = can_config->node1.data_bitrate,
                         .tx_queue_depth = CAN_TX_QUEUE_DEPTH, .listen_only = can_config->node1.listen_only},
        .node2_config = {.tx_pin = CAN2_TX_PIN, .rx_pin = CAN2_RX_PIN,
                         .mode = can_config->node2.mode == CAN_MODE_FD ? TWAI_BUS_MODE_FD : TWAI_BUS_MODE_CLASSIC,
                         .arbitration_bitrate = can_config->node2.arbitration_bitrate,
                         .data_bitrate = can_config->node2.data_bitrate,
                         .tx_queue_depth = CAN_TX_QUEUE_DEPTH, .listen_only = can_config->node2.listen_only},
        .log_ringbuf = buffers.log_ringbuf,
        .route_ringbuf = bridge_capable ? buffers.route_ringbuf : NULL,
        .route_use_fd_frames = can_config->node2.mode == CAN_MODE_FD,
        .route_tx_count = &metrics->tx_frames_node2,
        .log_drop_count = &metrics->log_drops,
        .route_drop_count = &metrics->gateway_drops,
        .rx_count = &metrics->rx_frames,
        .rx_count_node1 = &metrics->rx_frames_node1,
        .rx_count_node2 = &metrics->rx_frames_node2,
        .log_ringbuf_in_count = &metrics->ringbuf_in,
    };
    ESP_RETURN_ON_ERROR(can_services_configure(&can_services_config), "app_setup", "configure CAN services");
    ESP_RETURN_ON_ERROR(can_services_init_nodes(), "app_setup", "initialize CAN nodes");
    ESP_RETURN_ON_ERROR(can_services_start(NULL), "app_setup", "start CAN services");

    const app_stats_ui_config_t stats_ui_config = {
        .oled_enabled = peripherals->oled_available,
        .pcf8574 = &peripherals->pcf8574,
        .metrics = metrics,
    };
    app_stats_ui_start(&stats_ui_config);

    if (tcp_needed) {
        const tcp_service_config_t tcp_config = {
            .tx_port = TCP_TX_PORT,
            .rx_port = TCP_RX_PORT,
            .ringbuf = buffers.log_ringbuf,
            .node1 = can_services_get_node(1),
            .node2 = can_services_get_node(2),
            .use_fd_frames = can_config->node2.mode == CAN_MODE_FD,
            .tx_frames_node1 = &metrics->tx_frames_node1,
            .tx_frames_node2 = &metrics->tx_frames_node2,
            .ringbuf_out = &metrics->ringbuf_out,
        };
        ESP_RETURN_ON_ERROR(tcp_service_start(&tcp_config), "app_setup", "start TCP service");
    }

    return ESP_OK;
}