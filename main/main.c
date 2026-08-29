#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
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

#include "driver/i2c_master.h"
#include "c_oled.h"
#include "ds3231mz.h"
#include "pcf8574.h"
#include "twai_manager.h"
#include "sdcard_service.h"

static const char *TAG = "app_main";

typedef enum {
    APP_MODE_BRIDGE_ONLY = 0,
    APP_MODE_SD_LOGGER,
    APP_MODE_TCP_SERVER
} app_mode_t;

app_mode_t g_active_app_mode = APP_MODE_BRIDGE_ONLY;

#define CAN1_BITRATE 1000000
#define CAN1_DATA_BITRATE 5000000

#define CAN2_BITRATE 1000000
#define CAN2_DATA_BITRATE 2000000

#define TX_PORT 3333
#define RX_PORT 3334

#define TX_PIN_NODE_1 GPIO_NUM_23
#define RX_PIN_NODE_1 GPIO_NUM_24
#define TX_PIN_NODE_2 GPIO_NUM_4
#define RX_PIN_NODE_2 GPIO_NUM_5

#define PIN_NUM_MOSI GPIO_NUM_8
#define PIN_NUM_MISO GPIO_NUM_9
#define PIN_NUM_CLK GPIO_NUM_10
#define PIN_NUM_CS GPIO_NUM_6

#define I2C_PORT_NUM I2C_NUM_0
#define I2C_SDA_GPIO GPIO_NUM_0
#define I2C_SCL_GPIO GPIO_NUM_1
#define I2C_FREQ_HZ 400000
#define OLED_I2C_ADDR 0x3C
#define PCF8574_I2C_ADDR 0x20

#define TWAI_USE_FD_FRAMES 1

#define FRAMES_PER_PACKET 40
#define SD_BLOCK_SIZE (16 * 1024)

#define DRIVER_TX_QUEUE_DEPTH 256
#define EJECT_BTN_PIN GPIO_NUM_28

typedef struct __attribute__((packed))
{
    uint32_t timestamp;
    uint8_t node_id;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[64];
} log_frame_t;

typedef struct __attribute__((packed))
{
    uint32_t timestamp;
    uint8_t node_id;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[64];
} tcp_cmd_frame_t;

RingbufHandle_t log_ringbuf = NULL;
RingbufHandle_t gateway_ringbuf = NULL;
#define LOG_RINGBUF_SIZE (3 * 1024 * 1024)
#define GATEWAY_RINGBUF_SIZE (3 * 1024 * 1024)

static RingbufHandle_t create_psram_ringbuf(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    StaticRingbuffer_t *struct_ptr = heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf && struct_ptr)
    {
        return xRingbufferCreateStatic(size, RINGBUF_TYPE_NOSPLIT, buf, struct_ptr);
    }
    return NULL;
}

static twai_mgr_inst_t node1 = {0};
static twai_mgr_inst_t node2 = {0};
static ds3231mz_t s_rtc = {0};
static pcf8574_t s_pcf8574 = {0};

static volatile bool network_ready = false;
static char g_ip_str[16] = "";
static bool s_oled_available = false;
static volatile bool g_stop_logging = false;
static volatile bool g_pc_connected = false;

static volatile uint32_t g_rx_frames = 0;
static volatile uint32_t g_rx_frames_node1 = 0;
static volatile uint32_t g_rx_frames_node2 = 0;
static volatile uint32_t g_tx_frames_node1 = 0;
static volatile uint32_t g_tx_frames_node2 = 0;

static volatile uint32_t g_drop_log = 0;
static volatile uint32_t g_drop_gw = 0;
static volatile uint32_t g_tcp_rx_ok = 0;

static volatile uint32_t g_rb_in = 0;
static volatile uint32_t g_rb_out = 0;
static volatile uint32_t g_tcp_sent = 0;

static void IRAM_ATTR eject_btn_isr_handler(void *arg)
{
    g_stop_logging = true;
}

static void app_can_rx_handler(uint8_t node_id, const twai_frame_t *rx_frame, void *user_ctx)
{
    if (!rx_frame) return;

    log_frame_t frame = {0};

    frame.timestamp = (uint32_t)(rx_frame->header.timestamp / 1000);
    frame.node_id = node_id;
    frame.id = rx_frame->header.id;
    if (rx_frame->header.ide)
        frame.id |= 0x80000000U;

    frame.dlc = rx_frame->header.dlc;
    
    uint8_t copy_len = twai_mgr_dlc_to_len(rx_frame->header.dlc);
    if (copy_len > 64) copy_len = 64;

    if (rx_frame->buffer && copy_len > 0)
    {
        uint8_t actual_len = (rx_frame->buffer_len < copy_len) ? rx_frame->buffer_len : copy_len;
        memcpy(frame.data, rx_frame->buffer, actual_len);
    }

    BaseType_t awoken = pdFALSE;

    if (g_active_app_mode == APP_MODE_BRIDGE_ONLY)
    {
        if (node_id == 1 && gateway_ringbuf)
        {
            if (xRingbufferSendFromISR(gateway_ringbuf, &frame, sizeof(frame), &awoken) != pdTRUE)
            {
                g_drop_gw++;
            }
        }
    }
    else
    {
        if (log_ringbuf)
        {
            if (xRingbufferSendFromISR(log_ringbuf, &frame, sizeof(frame), &awoken) != pdTRUE)
            {
                g_drop_log++;
            }
            else
            {
                g_rb_in++;
            }
        }
    }

    if (node_id == 1)
        g_rx_frames_node1++;
    else if (node_id == 2)
        g_rx_frames_node2++;

    g_rx_frames++;
    if (awoken == pdTRUE)
        portYIELD_FROM_ISR();
}

void can_gateway_task(void *pvParameters)
{
    log_frame_t gw_frame;
    size_t item_size;

    while (1)
    {
        void *data = xRingbufferReceive(gateway_ringbuf, &item_size, portMAX_DELAY);
        if (data != NULL)
        {
            memcpy(&gw_frame, data, sizeof(log_frame_t));
            vRingbufferReturnItem(gateway_ringbuf, data);

            if (!node2.recovery_in_progress)
            {
                uint8_t safe_dlc = twai_mgr_bound_dlc(gw_frame.dlc);
                twai_frame_t tx_frame = {
                    .header = {
                        .id = gw_frame.id & 0x1FFFFFFFU, 
                        .ide = (gw_frame.id & 0x80000000U) ? 1 : 0, 
                        .fdf = TWAI_USE_FD_FRAMES, 
                        .brs = TWAI_USE_FD_FRAMES ? 1 : 0, 
                        .esi = 0, 
                        .dlc = TWAI_USE_FD_FRAMES ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc)
                    },
                    .buffer = gw_frame.data,
                    .buffer_len = TWAI_USE_FD_FRAMES ? twai_mgr_dlc_to_len(safe_dlc) : ((twai_mgr_dlc_to_len(safe_dlc) > 8) ? 8 : twai_mgr_dlc_to_len(safe_dlc))
                };

                esp_err_t err = twai_mgr_async_transmit(&node2, &tx_frame, 0);
                if (err == ESP_OK)
                {
                    g_tx_frames_node2++;
                }
            }
        }
    }
}

void tcp_sender_task(void *pvParameters)
{
    while (!network_ready)
        vTaskDelay(pdMS_TO_TICKS(500));

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0)
        vTaskDelete(NULL);

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(TX_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(listen_sock, (struct sockaddr *)&address, sizeof(address));
    listen(listen_sock, 1);

    ESP_LOGI("TCP_SERVER", "TCP server ready on port %d", TX_PORT);

    log_frame_t packet_buffer[FRAMES_PER_PACKET];
    size_t item_size;

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        g_pc_connected = true;
        int frame_count = 0;
        int burst_counter = 0;

        while (1)
        {
            TickType_t wait_time = (frame_count == 0) ? portMAX_DELAY : pdMS_TO_TICKS(50);
            void *data = xRingbufferReceive(log_ringbuf, &item_size, wait_time);

            if (data != NULL)
            {
                memcpy(&packet_buffer[frame_count++], data, sizeof(log_frame_t));
                vRingbufferReturnItem(log_ringbuf, data);
                g_rb_out++;

                if (frame_count >= FRAMES_PER_PACKET)
                {
                    int err = send(sock, packet_buffer, frame_count * sizeof(log_frame_t), 0);
                    if (err < 0)
                    {
                        break;
                    }
                    g_tcp_sent += frame_count;
                    frame_count = 0;

                    if (++burst_counter >= 10)
                    {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        burst_counter = 0;
                    }
                }
            }
            else if (frame_count > 0)
            {
                int err = send(sock, packet_buffer, frame_count * sizeof(log_frame_t), 0);
                if (err < 0)
                    break;
                g_tcp_sent += frame_count;
                frame_count = 0;
            }
        }

        g_pc_connected = false;
        close(sock);
    }
}

void tcp_receiver_task(void *pvParameters)
{
    while (!network_ready)
        vTaskDelay(pdMS_TO_TICKS(500));

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in listen_addr = {.sin_family = AF_INET, .sin_port = htons(RX_PORT)};
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
    listen(listen_sock, 1);

    while (1)
    {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        tcp_cmd_frame_t cmd;
        uint8_t *rx_buf = (uint8_t *)&cmd;
        int rx_bytes = 0;

        while (1)
        {
            int len = recv(sock, rx_buf + rx_bytes, sizeof(cmd) - rx_bytes, 0);
            if (len <= 0)
                break;

            rx_bytes += len;
            if (rx_bytes == sizeof(cmd))
            {
                rx_bytes = 0;
                uint8_t safe_dlc = twai_mgr_bound_dlc(cmd.dlc);
                twai_frame_t tx_frame = {
                    .header = {
                        .id = cmd.id & 0x1FFFFFFFU, .ide = (cmd.id & 0x80000000U) ? 1 : 0, .fdf = TWAI_USE_FD_FRAMES, .brs = TWAI_USE_FD_FRAMES ? 1 : 0, .esi = 0, .dlc = TWAI_USE_FD_FRAMES ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc)},
                    .buffer = cmd.data,
                    .buffer_len = TWAI_USE_FD_FRAMES ? twai_mgr_dlc_to_len(safe_dlc) : ((twai_mgr_dlc_to_len(safe_dlc) > 8) ? 8 : twai_mgr_dlc_to_len(safe_dlc))};

                twai_mgr_inst_t *target_node = (cmd.node_id == 1) ? &node1 : &node2;
                if (!target_node->recovery_in_progress)
                {
                    int retries = 0;
                    while (twai_mgr_async_transmit(target_node, &tx_frame, pdMS_TO_TICKS(20)) != ESP_OK)
                    {
                        vTaskDelay(pdMS_TO_TICKS(2));
                        if (++retries > 50) break;
                    }
                    if (retries <= 50)
                    {
                        if (cmd.node_id == 1)
                            g_tx_frames_node1++;
                        else
                            g_tx_frames_node2++;
                    }
                }
            }
        }
        close(sock);
    }
}

void twai_health_task(void *pvParameters)
{
    bool node1_was_recovering = false;
    bool node2_was_recovering = false;

    while (1)
    {
        twai_mgr_health_monitor(&node1);
        twai_mgr_health_monitor(&node2);

        if (node1.recovery_in_progress && !node1_was_recovering) {
            ESP_LOGE(TAG, "FAULT: CAN Bus 1");
        } else if (!node1.recovery_in_progress && node1_was_recovering) {
            ESP_LOGI(TAG, "SUCCESS: CAN Bus 1");
        }

        if (node2.recovery_in_progress && !node2_was_recovering) {
            ESP_LOGE(TAG, "FAULT: CAN Bus 2");
        } else if (!node2.recovery_in_progress && node2_was_recovering) {
            ESP_LOGI(TAG, "SUCCESS: CAN Bus 2");
        }

        node1_was_recovering = node1.recovery_in_progress;
        node2_was_recovering = node2.recovery_in_progress;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void diagnostics_task(void *pvParameters)
{
    while (1)
    {
        uint32_t in_buffer = g_rb_in - g_rb_out;

        printf("[SYSTEM] Mode: %d | RX: %lu | GW Drops: %lu | Backlog: %lu\n",
               (int)g_active_app_mode,
               (unsigned long)g_rx_frames,
               (unsigned long)g_drop_gw,
               (unsigned long)in_buffer);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void sd_writer_task(void *pvParameters)
{
    uint8_t *write_buffer = heap_caps_malloc(SD_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t current_bytes = 0;
    size_t item_size;

    char log_filename[64] = "/can_log.bin";
    ds3231mz_datetime_t now = {0};
    if (ds3231mz_read_time(&s_rtc, &now) == ESP_OK)
    {
        snprintf(log_filename, sizeof(log_filename), "/log_%04u%02u%02u_%02u%02u%02u.bin",
                 now.year, now.month, now.date, now.hour, now.minute, now.second);
    }

    while (1)
    {
        if (g_stop_logging)
        {
            if (current_bytes > 0)
            {
                sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes);
                current_bytes = 0;
            }
            sdcard_service_sync();
            sdcard_service_unmount();
            heap_caps_free(write_buffer);
            
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        void *data = xRingbufferReceive(log_ringbuf, &item_size, pdMS_TO_TICKS(1000));
        if (data != NULL)
        {
            g_rb_out++;
            if (current_bytes + item_size > SD_BLOCK_SIZE)
            {
                sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes);
                current_bytes = 0;
            }
            memcpy(write_buffer + current_bytes, data, item_size);
            current_bytes += item_size;
            vRingbufferReturnItem(log_ringbuf, data);
        }
        else
        {
            if (current_bytes > 0)
            {
                sdcard_service_append_bin_block(log_filename, write_buffer, current_bytes);
                current_bytes = 0;
            }
            sdcard_service_sync();
        }
    }
}

static void oled_display_task(void *arg)
{
    char text_buf[48];
    while (1)
    {
        uint8_t port_val = 0;
        if (pcf8574_read_byte(&s_pcf8574, &port_val) == ESP_OK)
        {
            port_val |= 0x0F;  
            port_val |= 0xF0;  

            if (g_active_app_mode == APP_MODE_BRIDGE_ONLY) {
                port_val &= ~(1 << 4);
            } else if (g_active_app_mode == APP_MODE_SD_LOGGER) {
                port_val &= ~(1 << 5);
            } else if (g_active_app_mode == APP_MODE_TCP_SERVER) {
                port_val &= ~(1 << 6);
            }

            pcf8574_write_byte(&s_pcf8574, port_val);
        }

        c_oled_clear_buffer();
        
        if (g_stop_logging)
        {
            c_oled_draw_string(0, 0, "SD Card Ejected");
            c_oled_draw_string(0, 2, "Safe to remove!");
            c_oled_draw_string(0, 4, "You can power off");
        }
        else
        {
            if (g_active_app_mode == APP_MODE_BRIDGE_ONLY)
                c_oled_draw_string(0, 0, "CAN Bridge Mode");
            else if (g_active_app_mode == APP_MODE_SD_LOGGER)
                c_oled_draw_string(0, 0, "SD Logger Mode");
            else
                c_oled_draw_string(0, 0, "TCP Server Mode");

            snprintf(text_buf, sizeof(text_buf), "C1 T:%lu R:%lu", (unsigned long)g_tx_frames_node1, (unsigned long)g_rx_frames_node1);
            c_oled_draw_string(0, 2, text_buf);

            snprintf(text_buf, sizeof(text_buf), "C2 T:%lu R:%lu", (unsigned long)g_tx_frames_node2, (unsigned long)g_rx_frames_node2);
            c_oled_draw_string(0, 4, text_buf);

            
            if (g_active_app_mode == APP_MODE_TCP_SERVER && network_ready)
                snprintf(text_buf, sizeof(text_buf), "%s", g_ip_str);
            else
                snprintf(text_buf, sizeof(text_buf), "GW Drops: %lu", (unsigned long)g_drop_gw);
            
            c_oled_draw_string(0, 6, text_buf);
        }

        c_oled_update();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        network_ready = false;
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        sprintf(g_ip_str, IPSTR, IP2STR(&event->ip_info.ip));
        network_ready = true;
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

esp_err_t sync_system_time_from_rtc(ds3231mz_t *rtc_dev)
{
    ds3231mz_datetime_t dt;
    esp_err_t err = ds3231mz_read_time(rtc_dev, &dt);
    if (err != ESP_OK)
        return err;

    struct tm tinfo = {0};
    tinfo.tm_year = dt.year - 1900;
    tinfo.tm_mon = dt.month - 1;
    tinfo.tm_mday = dt.date;
    tinfo.tm_hour = dt.hour;
    tinfo.tm_min = dt.minute;
    tinfo.tm_sec = dt.second;
    tinfo.tm_isdst = -1;

    time_t sys_time = mktime(&tinfo);
    struct timeval now = {.tv_sec = sys_time, .tv_usec = 0};
    settimeofday(&now, NULL);
    return ESP_OK;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_flash_init();
    esp_log_level_set("esp_twai", ESP_LOG_WARN);

    log_ringbuf = create_psram_ringbuf(LOG_RINGBUF_SIZE);
    gateway_ringbuf = create_psram_ringbuf(GATEWAY_RINGBUF_SIZE);
    if (!log_ringbuf || !gateway_ringbuf)
    {
        abort();
    }

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    if (i2c_master_probe(i2c_bus, OLED_I2C_ADDR, 50) == ESP_OK)
    {
        if (c_oled_init(i2c_bus) == ESP_OK)
            s_oled_available = true;
    }
    if (ds3231mz_init(&s_rtc, i2c_bus, 0, I2C_FREQ_HZ, 50) == ESP_OK)
    {
        bool power_lost = false;
        ds3231mz_get_power_lost(&s_rtc, &power_lost);
        if (power_lost)
        {
            ds3231mz_datetime_t def_time = {.year = 2026, .month = 8, .date = 26, .day = 3, .hour = 12, .minute = 0, .second = 0};
            ds3231mz_set_time(&s_rtc, &def_time);
            ds3231mz_clear_power_lost(&s_rtc);
        }
        sync_system_time_from_rtc(&s_rtc);
    }
    
    ESP_ERROR_CHECK(pcf8574_init(&s_pcf8574, i2c_bus, PCF8574_I2C_ADDR, I2C_FREQ_HZ, 50, 0xFF));

    uint8_t dip_state = 0xFF;
    if (pcf8574_read_byte(&s_pcf8574, &dip_state) == ESP_OK)
    {
        if ((dip_state & (1 << 0)) == 0) {
            g_active_app_mode = APP_MODE_BRIDGE_ONLY;
        } 
        else if ((dip_state & (1 << 1)) == 0) {
            g_active_app_mode = APP_MODE_SD_LOGGER;
        } 
        else if ((dip_state & (1 << 2)) == 0) {
            g_active_app_mode = APP_MODE_TCP_SERVER;
        } 
        else {
            g_active_app_mode = APP_MODE_BRIDGE_ONLY;
        }
    }

    if (g_active_app_mode == APP_MODE_SD_LOGGER)
    {
        sdcard_service_config_t sd_cfg = {
            .pin_mosi = PIN_NUM_MOSI, .pin_miso = PIN_NUM_MISO, .pin_sclk = PIN_NUM_CLK, .pin_cs = PIN_NUM_CS, .host_id = SPI2_HOST, .format_if_mount_failed = true, .max_files = 5, .mount_point = "/sdcard"};
        sdmmc_card_t *card;
        sdcard_service_mount(&sd_cfg, &card);
    }
    else if (g_active_app_mode == APP_MODE_TCP_SERVER)
    {
        wifi_init_sta();
    }

    ESP_ERROR_CHECK(twai_mgr_init_custom_node(&node1, TX_PIN_NODE_1, RX_PIN_NODE_1,
                                            TWAI_BUS_MODE_FD, CAN1_BITRATE, CAN1_DATA_BITRATE,
                                            DRIVER_TX_QUEUE_DEPTH, (void *)1, app_can_rx_handler));

    ESP_ERROR_CHECK(twai_mgr_init_custom_node(&node2, TX_PIN_NODE_2, RX_PIN_NODE_2,
                                            TWAI_BUS_MODE_FD, CAN2_BITRATE, CAN2_DATA_BITRATE,
                                            DRIVER_TX_QUEUE_DEPTH, (void *)2, app_can_rx_handler));

    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_NEGEDGE, .pin_bit_mask = (1ULL << EJECT_BTN_PIN), .mode = GPIO_MODE_INPUT, .pull_up_en = 1, .pull_down_en = 0};
    gpio_config(&btn_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(EJECT_BTN_PIN, eject_btn_isr_handler, NULL);

    if (g_active_app_mode == APP_MODE_BRIDGE_ONLY)
    {
        xTaskCreatePinnedToCore(can_gateway_task, "can_gw", 3072, NULL, 3, NULL, 0);
    }
    else if (g_active_app_mode == APP_MODE_SD_LOGGER)
    {
        xTaskCreatePinnedToCore(sd_writer_task, "sd_tx", 4096, NULL, 2, NULL, 0);
    }
    else if (g_active_app_mode == APP_MODE_TCP_SERVER)
    {
        xTaskCreatePinnedToCore(tcp_sender_task, "tcp_tx", 4096, NULL, 4, NULL, 0);
        xTaskCreatePinnedToCore(tcp_receiver_task, "tcp_rx", 4096, NULL, 4, NULL, 0);
    }

    xTaskCreatePinnedToCore(twai_health_task, "twai_health", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(diagnostics_task, "diag", 3072, NULL, 2, NULL, 0);
    
    if (s_oled_available)
        xTaskCreatePinnedToCore(oled_display_task, "oled", 4096, NULL, 2, NULL, 0);
}