#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include <arpa/inet.h>
#include "esp_heap_caps.h" 
#include "secrets.h"

// --- Custom Components ---
#include "driver/i2c_master.h"
#include "c_oled.h"
#include "ds3231mz.h"
#include "pcf8574.h"
#include "twai_manager.h" // Modular CAN driver manager

static const char *TAG = "app_main";

// ============================================================================
// UDP NETWORK CONFIGURATION
// ============================================================================

#define HOST_IP_ADDR "192.168.178.61" 
#define TX_PORT 3333    
#define RX_PORT 3334    

// ============================================================================
// CAN & I2C CONFIGURATION
// ============================================================================
#define TX_PIN_NODE_1 GPIO_NUM_4
#define RX_PIN_NODE_1 GPIO_NUM_5
#define TX_PIN_NODE_2 GPIO_NUM_23
#define RX_PIN_NODE_2 GPIO_NUM_24

#define I2C_PORT_NUM        I2C_NUM_0
#define I2C_SDA_GPIO        GPIO_NUM_0
#define I2C_SCL_GPIO        GPIO_NUM_1
#define I2C_FREQ_HZ         400000
#define OLED_I2C_ADDR       0x3C
#define PCF8574_I2C_ADDR    0x20

#define TWAI_USE_FD_FRAMES 1

// CAN Generator Parameters
#define GENERATOR_FRAMES_PER_BURST 1000
#define GENERATOR_BURST_INTERVAL_MS 2000
#define FRAMES_PER_PACKET 20

// Internal Hardware TX Queue Depth
#define DRIVER_TX_QUEUE_DEPTH 2048

// ============================================================================
// DATA STRUCTURES
// ============================================================================
typedef struct __attribute__((packed)) {
    uint32_t timestamp; 
    uint8_t node_id;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[64];
} log_frame_t;

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint8_t node_id;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[64];
} udp_cmd_frame_t;

// --- MASSIVE RX POOL ARCHITECTURE (Allocated in 8MB PSRAM) ---
#define POOL_SIZE 4096
typedef struct {
    log_frame_t frame;
    uint8_t ref_count;
} pool_item_t;

static pool_item_t *frame_pool = NULL;
static portMUX_TYPE pool_mux = portMUX_INITIALIZER_UNLOCKED;

QueueHandle_t free_queue;
QueueHandle_t log_queue;
QueueHandle_t routing_queue;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
static twai_mgr_inst_t node1 = {0};
static twai_mgr_inst_t node2 = {0};

static ds3231mz_t s_rtc = {0};
static pcf8574_t s_pcf8574 = {0};

static volatile bool network_ready = false;
static char g_ip_str[16] = ""; 
static bool s_oled_available = false;

static volatile uint32_t g_rx_frames = 0;
static volatile uint32_t g_rx_queue_drop = 0;
static volatile uint32_t g_udp_tx_ok = 0;
static volatile uint32_t g_gen_tx_ok = 0;

static volatile uint32_t g_rx_fps = 0;
static volatile uint32_t g_tx_fps = 0;
static volatile bool g_error_led_trigger = false; 
static volatile bool enable_test_generator = true;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================
static void release_frame(uint16_t idx) {
    bool return_to_pool = false;
    taskENTER_CRITICAL(&pool_mux);
    if (frame_pool[idx].ref_count > 0) {
        frame_pool[idx].ref_count--;
        if (frame_pool[idx].ref_count == 0) return_to_pool = true;
    }
    taskEXIT_CRITICAL(&pool_mux);
    if (return_to_pool) xQueueSend(free_queue, &idx, 0);
}

// ============================================================================
// APPLICATION-LEVEL CAN RX CALLBACK (Fed from twai_manager)
// ============================================================================
static void app_can_rx_handler(uint8_t node_id, const twai_frame_t *rx_frame, void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    uint16_t free_idx;

    if (xQueueReceiveFromISR(free_queue, &free_idx, &high_task_wakeup) == pdTRUE) {
        pool_item_t *item = &frame_pool[free_idx];
        
       
        item->frame.timestamp = (uint32_t)(rx_frame->header.timestamp / 1000);
        
        item->frame.node_id = node_id;
        item->frame.id = rx_frame->header.id;
        if (rx_frame->header.ide) item->frame.id |= 0x80000000U;

        item->frame.dlc = rx_frame->header.dlc;
        uint8_t copy_len = twai_mgr_dlc_to_len(rx_frame->header.dlc);
        memcpy(item->frame.data, rx_frame->buffer, (copy_len > 64) ? 64 : copy_len);

        item->ref_count = 2; // Sent to both log_queue and routing_queue
        g_rx_frames++;

        if (xQueueSendFromISR(log_queue, &free_idx, &high_task_wakeup) != pdTRUE) {
            item->ref_count--; g_rx_queue_drop++;
        }
        if (xQueueSendFromISR(routing_queue, &free_idx, &high_task_wakeup) != pdTRUE) {
            item->ref_count--; g_rx_queue_drop++;
        }
        
        if (item->ref_count == 0) xQueueSendFromISR(free_queue, &free_idx, &high_task_wakeup);
    } else {
        g_rx_queue_drop++; 
    }
}

// ============================================================================
// SYSTEM TASKS
// ============================================================================

void udp_sender_task(void *pvParameters) {
    while (!network_ready) vTaskDelay(pdMS_TO_TICKS(500));

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in dest_addr = { .sin_family = AF_INET, .sin_port = htons(TX_PORT) };
    dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);

    log_frame_t packet_buffer[FRAMES_PER_PACKET];
    int frame_count = 0, burst_counter = 0;

    while (1) {
        uint16_t idx;
        TickType_t wait_time = (frame_count == 0) ? portMAX_DELAY : pdMS_TO_TICKS(50);
        
        if (xQueueReceive(log_queue, &idx, wait_time) == pdTRUE) {
            packet_buffer[frame_count++] = frame_pool[idx].frame;
            release_frame(idx);
            
            if (frame_count >= FRAMES_PER_PACKET) {
                sendto(sock, packet_buffer, sizeof(packet_buffer), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                
                if (++burst_counter >= 10) { 
                    taskYIELD(); 
                    burst_counter = 0; 
                }
                frame_count = 0;
            }
        } else if (frame_count > 0) {
            sendto(sock, packet_buffer, frame_count * sizeof(log_frame_t), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            burst_counter = 0; frame_count = 0;
        }
    }
}

void udp_receiver_task(void *pvParameters) {
    while (!network_ready) vTaskDelay(pdMS_TO_TICKS(500));
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in listen_addr = { .sin_family = AF_INET, .sin_port = htons(RX_PORT) };
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));

    udp_cmd_frame_t cmd;
    while (1) {
        if (recvfrom(sock, &cmd, sizeof(cmd), 0, NULL, NULL) < 10) continue;

        uint8_t safe_dlc = twai_mgr_bound_dlc(cmd.dlc);
        twai_frame_t tx_frame = {
            .header = {
                .id = cmd.id & 0x1FFFFFFFU, .ide = (cmd.id & 0x80000000U) ? 1 : 0,
                .fdf = TWAI_USE_FD_FRAMES, .brs = TWAI_USE_FD_FRAMES ? 1 : 0,
                .esi = 0, .dlc = TWAI_USE_FD_FRAMES ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc)
            },
            .buffer = cmd.data, 
            .buffer_len = TWAI_USE_FD_FRAMES ? twai_mgr_dlc_to_len(safe_dlc) : ((twai_mgr_dlc_to_len(safe_dlc) > 8) ? 8 : twai_mgr_dlc_to_len(safe_dlc))
        };

        twai_mgr_inst_t *target_node = (cmd.node_id == 1) ? &node1 : &node2;
        
        if (target_node->recovery_in_progress) continue;

        while (twai_mgr_async_transmit(target_node, &tx_frame, pdMS_TO_TICKS(20)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1)); 
        }
        g_udp_tx_ok++;
    }
}

void osci_tx_task(void *pvParameters) {
    while (!network_ready) vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "CAN Generator ready to blast");
    
    uint8_t can_data[64];
    for (int i = 0; i < 64; i++) can_data[i] = (i % 2 == 0) ? 0xAA : 0x55;

    twai_frame_t osci_frame = {
        .header = { 
            .id = 0x55, .ide = 0, .fdf = TWAI_USE_FD_FRAMES,
            .brs = TWAI_USE_FD_FRAMES ? 1 : 0, .esi = 0,
            .dlc = TWAI_USE_FD_FRAMES ? 15 : 8 
        },
        .buffer = can_data, .buffer_len = TWAI_USE_FD_FRAMES ? 64 : 8
    };

    twai_mgr_inst_t *nodes[] = { &node1, &node2 };
    const uint32_t generator_ids[] = { 0x55, 0x66 };

    while (1) {
        if (!enable_test_generator) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        for (size_t node_index = 0; node_index < 2; node_index++) {
            if (nodes[node_index]->recovery_in_progress) continue;

            osci_frame.header.id = generator_ids[node_index];
            for (int i = 0; i < GENERATOR_FRAMES_PER_BURST; i++) {
                
                esp_err_t err = twai_mgr_async_transmit(nodes[node_index], &osci_frame, pdMS_TO_TICKS(50));
                if (err == ESP_OK) {
                    g_gen_tx_ok++;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    break;
                }

                if ((i % 50) == 49) {
                    taskYIELD(); 
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(GENERATOR_BURST_INTERVAL_MS));
    }
}

void can_routing_task(void *pvParameters) {
    uint16_t idx;
    while (xQueueReceive(routing_queue, &idx, portMAX_DELAY) == pdTRUE) {
        release_frame(idx);
    }
}

// ============================================================================
// HEALTH & DIAGNOSTICS TASKS
// ============================================================================
void twai_health_task(void *pvParameters) {
    while (1) {
        twai_mgr_health_monitor(&node1);
        twai_mgr_health_monitor(&node2);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void diagnostics_task(void *pvParameters) {
    while (1) {
        twai_node_status_t st1 = {0}; twai_node_record_t rec1 = {0};
        twai_node_status_t st2 = {0}; twai_node_record_t rec2 = {0};
        if (node1.handle) twai_node_get_info(node1.handle, &st1, &rec1);
        if (node2.handle) twai_node_get_info(node2.handle, &st2, &rec2);

        printf("[DIAG] FPS(rx/tx)=%lu/%lu drop=%lu gen=%lu udp=%lu | N1(err_tx/rx)=%lu/%lu N2=%lu/%lu\n",
               (unsigned long)g_rx_fps, (unsigned long)g_tx_fps,
               (unsigned long)g_rx_queue_drop,
               (unsigned long)g_gen_tx_ok, (unsigned long)g_udp_tx_ok,
               (unsigned long)st1.tx_error_count, (unsigned long)st1.rx_error_count,
               (unsigned long)st2.tx_error_count, (unsigned long)st2.rx_error_count);
               
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================================
// PERIPHERALS TASKS (Stats, LEDs, OLED)
// ============================================================================
static void stats_calc_task(void *arg) {
    uint32_t last_rx = 0, last_tx = 0;
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        g_rx_fps = g_rx_frames - last_rx;
        g_tx_fps = g_gen_tx_ok - last_tx;
        last_rx = g_rx_frames; last_tx = g_gen_tx_ok;
    }
}

static void oled_display_task(void *arg) {
    char text_buf[32];
    ds3231mz_datetime_t now = {0};
    uint32_t last_drop_count = 0;

    while (1) {
        c_oled_clear_buffer();

        if (network_ready) snprintf(text_buf, sizeof(text_buf), "%s", g_ip_str);
        else snprintf(text_buf, sizeof(text_buf), "Scanning WiFi...");
        c_oled_draw_string(0, 0, text_buf);

        if (ds3231mz_read_time(&s_rtc, &now) == ESP_OK) {
            snprintf(text_buf, sizeof(text_buf), "RTC %02u:%02u:%02u", now.hour, now.minute, now.second);
        } else snprintf(text_buf, sizeof(text_buf), "RTC ERROR");
        c_oled_draw_string(0, 2, text_buf);

        snprintf(text_buf, sizeof(text_buf), "RX/s:%lu TX/s:%lu", (unsigned long)g_rx_fps, (unsigned long)g_tx_fps);
        c_oled_draw_string(0, 4, text_buf);

        if (g_rx_queue_drop > last_drop_count) {
            g_error_led_trigger = true; 
            last_drop_count = g_rx_queue_drop;
        }
        snprintf(text_buf, sizeof(text_buf), "Dropped: %lu", (unsigned long)g_rx_queue_drop);
        c_oled_draw_string(0, 6, text_buf);

        c_oled_update();
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

// ============================================================================
// SYSTEM & WIFI INITIALIZATION
// ============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) { network_ready = false; esp_wifi_connect(); }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        sprintf(g_ip_str, IPSTR, IP2STR(&event->ip_info.ip));
        network_ready = true; 
        ESP_LOGI(TAG, "Connected! IP: %s", g_ip_str);
    }
}

static void wifi_init_sta(void) {
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
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_flash_init();
    esp_log_level_set("esp_twai", ESP_LOG_WARN);

    // 1. Dynamic Allocation of Memory Pools in PSRAM
    ESP_LOGI(TAG, "Allocating massive memory pools in PSRAM...");
    frame_pool = (pool_item_t *)heap_caps_calloc(POOL_SIZE, sizeof(pool_item_t), MALLOC_CAP_SPIRAM);
    if (!frame_pool) {
        ESP_LOGE(TAG, "FATAL: Not enough PSRAM for CAN pools! Halting.");
        abort();
    }

    // 2. I2C, OLED, RTC, PCF8574
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT_NUM, .sda_io_num = I2C_SDA_GPIO, .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));
    
    if (i2c_master_probe(i2c_bus, OLED_I2C_ADDR, 50) == ESP_OK) {
        if (c_oled_init(i2c_bus) == ESP_OK) s_oled_available = true;
    }
    if (ds3231mz_init(&s_rtc, i2c_bus, 0, I2C_FREQ_HZ, 50) == ESP_OK) {
        bool power_lost = false; ds3231mz_get_power_lost(&s_rtc, &power_lost);
        if (power_lost) {
            ds3231mz_datetime_t def_time = { .year = 2026, .month = 8, .date = 23, .day = 7, .hour = 12, .minute = 0, .second = 0 };
            ds3231mz_set_time(&s_rtc, &def_time); ds3231mz_clear_power_lost(&s_rtc);
        }
    }
    ESP_ERROR_CHECK(pcf8574_init(&s_pcf8574, i2c_bus, PCF8574_I2C_ADDR, I2C_FREQ_HZ, 50, 0xFF));

    // 3. System Queues
    free_queue = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    log_queue = xQueueCreate(POOL_SIZE, sizeof(uint16_t));
    routing_queue = xQueueCreate(POOL_SIZE, sizeof(uint16_t)); 

    for (uint16_t i = 0; i < POOL_SIZE; i++) xQueueSend(free_queue, &i, portMAX_DELAY);

    // 4. WiFi
    wifi_init_sta();

    // 5. Initialize CAN Nodes using the custom Manager initializer (Mode, Speeds, Pins set externally)
    // Node 1: CAN FD (1 Mbps Arbitration, 5 Mbps Data Phase)
    ESP_ERROR_CHECK(twai_mgr_init_custom_node(&node1, TX_PIN_NODE_1, RX_PIN_NODE_1, 
                                               TWAI_BUS_MODE_FD, 1000000, 5000000, 
                                               DRIVER_TX_QUEUE_DEPTH, (void*)1, app_can_rx_handler));

    // Node 2: CAN FD (1 Mbps Arbitration, 5 Mbps Data Phase)
    ESP_ERROR_CHECK(twai_mgr_init_custom_node(&node2, TX_PIN_NODE_2, RX_PIN_NODE_2, 
                                               TWAI_BUS_MODE_FD, 1000000, 5000000, 
                                               DRIVER_TX_QUEUE_DEPTH, (void*)2, app_can_rx_handler));

    // 6. Core System Tasks 
    xTaskCreatePinnedToCore(udp_sender_task, "udp_tx", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(udp_receiver_task, "udp_rx", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(osci_tx_task, "osci_tx", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(twai_health_task, "twai_health", 3072, NULL, 4, NULL, 0); 
    xTaskCreatePinnedToCore(can_routing_task, "can_route", 4096, NULL, 3, NULL, 0);
    
    // 7. Peripherals Tasks
    xTaskCreatePinnedToCore(diagnostics_task, "diag", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(stats_calc_task, "stats_task", 2048, NULL, 2, NULL, 0);
    if (s_oled_available) xTaskCreatePinnedToCore(oled_display_task, "oled", 4096, NULL, 2, NULL, 0);
}