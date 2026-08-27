#include <string.h>
#include "udp_service.h"
#include "wifi_manager.h"
#include "lwip/sockets.h"
#include <arpa/inet.h>
#include "esp_log.h"
#include "freertos/task.h"

#define FRAMES_PER_PACKET 20

static udp_service_config_t s_cfg = {0};
static volatile uint32_t s_udp_tx_ok = 0;

uint32_t udp_service_get_tx_ok_count(void) {
    return s_udp_tx_ok;
}

static void udp_sender_task(void *pvParameters) {
    while (!wifi_manager_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    
    int sndbuf = 32 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in dest_addr = { .sin_family = AF_INET, .sin_port = htons(s_cfg.tx_port) };
    dest_addr.sin_addr.s_addr = inet_addr(s_cfg.host_ip);

    log_frame_t packet_buffer[FRAMES_PER_PACKET];
    int frame_count = 0, burst_counter = 0;
    size_t item_size;

    while (1) {
        TickType_t wait_time = (frame_count == 0) ? portMAX_DELAY : pdMS_TO_TICKS(50);
        
        void *data = xRingbufferReceive(s_cfg.tx_ringbuf, &item_size, wait_time);
        
        if (data != NULL) {
            memcpy(&packet_buffer[frame_count++], data, sizeof(log_frame_t));
            vRingbufferReturnItem(s_cfg.tx_ringbuf, data);
            
            if (frame_count >= FRAMES_PER_PACKET) {
                sendto(sock, packet_buffer, frame_count * sizeof(log_frame_t), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                
                if (++burst_counter >= 10) { 
                    taskYIELD(); 
                    burst_counter = 0; 
                }
                frame_count = 0;
            }
        } else if (frame_count > 0) {
            sendto(sock, packet_buffer, frame_count * sizeof(log_frame_t), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            burst_counter = 0; 
            frame_count = 0;
        }
    }
}

static void udp_receiver_task(void *pvParameters) {
    while (!wifi_manager_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in listen_addr = { .sin_family = AF_INET, .sin_port = htons(s_cfg.rx_port) };
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));

    udp_cmd_frame_t cmd;
    while (1) {
        if (recvfrom(sock, &cmd, sizeof(cmd), 0, NULL, NULL) < 10) continue;

        uint8_t safe_dlc = twai_mgr_bound_dlc(cmd.dlc);
        twai_frame_t tx_frame = {
            .header = {
                .id = cmd.id & 0x1FFFFFFFU, .ide = (cmd.id & 0x80000000U) ? 1 : 0,
                .fdf = s_cfg.use_fd_frames, .brs = s_cfg.use_fd_frames ? 1 : 0,
                .esi = 0, .dlc = s_cfg.use_fd_frames ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc)
            },
            .buffer = cmd.data, 
            .buffer_len = s_cfg.use_fd_frames ? twai_mgr_dlc_to_len(safe_dlc) : ((twai_mgr_dlc_to_len(safe_dlc) > 8) ? 8 : twai_mgr_dlc_to_len(safe_dlc))
        };

        twai_mgr_inst_t *target_node = (cmd.node_id == 1) ? s_cfg.node1 : s_cfg.node2;
        if (!target_node || target_node->recovery_in_progress) continue;

        while (twai_mgr_async_transmit(target_node, &tx_frame, pdMS_TO_TICKS(20)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1)); 
        }

        s_udp_tx_ok++;
    }
}

esp_err_t udp_service_start(const udp_service_config_t *config) {
    if (!config || !config->host_ip) return ESP_ERR_INVALID_ARG;
    s_cfg = *config;

    if (s_cfg.tx_ringbuf) {
        xTaskCreatePinnedToCore(udp_sender_task, "udp_tx", 4096, NULL, 4, NULL, 0); 
    }
    xTaskCreatePinnedToCore(udp_receiver_task, "udp_rx", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}
