#include <errno.h>
#include <string.h>
#include "tcp_service.h"
#include "app_types.h"
#include "wifi_manager.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <arpa/inet.h>
#include "esp_log.h"
#include "freertos/task.h"

#define FRAMES_PER_PACKET 40
#define RX_BUF_COUNT 16

static tcp_service_config_t s_cfg = {0};

static bool send_all(int sock, const void *data, size_t size)
{
    const uint8_t *buffer = data;
    size_t sent = 0;

    while (sent < size) {
        int result = send(sock, buffer + sent, size - sent, 0);
        if (result <= 0) {
            return false;
        }
        sent += result;
    }
    return true;
}

static void tcp_sender_task(void *pvParameters)
{
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(s_cfg.tx_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(listen_sock, (struct sockaddr *)&address, sizeof(address));
    listen(listen_sock, 1);

    ESP_LOGI("TCP_SERVER", "TCP server ready on port %d", s_cfg.tx_port);

    log_frame_t packet_buffer[FRAMES_PER_PACKET];
    size_t item_size;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int frame_count = 0;
        int burst_counter = 0;

        while (1) {
            TickType_t wait_time = frame_count == 0 ? portMAX_DELAY : pdMS_TO_TICKS(50);
            void *data = xRingbufferReceive(s_cfg.ringbuf, &item_size, wait_time);

            if (data != NULL) {
                memcpy(&packet_buffer[frame_count++], data, sizeof(log_frame_t));
                vRingbufferReturnItem(s_cfg.ringbuf, data);
                if (s_cfg.ringbuf_out) {
                    (*s_cfg.ringbuf_out)++;
                }

                if (frame_count >= FRAMES_PER_PACKET) {
                    if (!send_all(sock, packet_buffer, frame_count * sizeof(log_frame_t))) {
                        break;
                    }
                    frame_count = 0;

                    if (++burst_counter >= 10) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        burst_counter = 0;
                    }
                }
            } else if (frame_count > 0) {
                if (!send_all(sock, packet_buffer, frame_count * sizeof(log_frame_t))) {
                    break;
                }
                frame_count = 0;
            }
        }

        close(sock);
    }
}

static void tcp_receiver_task(void *pvParameters)
{
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE("TCP_RX", "Unable to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_cfg.rx_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE("TCP_RX", "Socket unable to bind");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_sock, 1);

    ESP_LOGI("TCP_RX", "TCP receiver ready on port %d", s_cfg.rx_port);

    udp_cmd_frame_t cmd_array[RX_BUF_COUNT];
    uint8_t cmd_idx = 0;

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI("TCP_RX", "PC Connected for sending CAN frames");

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int rx_bytes = 0;
        while (1) {
            udp_cmd_frame_t *cmd = &cmd_array[cmd_idx];
            uint8_t *rx_buf = (uint8_t *)cmd;
            int len = recv(sock, rx_buf + rx_bytes, sizeof(*cmd) - rx_bytes, 0);

            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!wifi_manager_is_connected()) {
                        ESP_LOGW("TCP_RX", "Wi-Fi lost, closing connection.");
                        break;
                    }
                    continue;
                }
                ESP_LOGE("TCP_RX", "Socket error: %d", errno);
                break;
            }
            if (len == 0) {
                ESP_LOGW("TCP_RX", "Connection gracefully closed by PC");
                break;
            }

            rx_bytes += len;
            if (rx_bytes == sizeof(*cmd)) {
                rx_bytes = 0;
                uint8_t safe_dlc = twai_mgr_bound_dlc(cmd->dlc);
                uint8_t payload_len = twai_mgr_dlc_to_len(safe_dlc);
                if (!s_cfg.use_fd_frames && payload_len > 8) {
                    payload_len = 8;
                }
                twai_frame_t tx_frame = {
                    .header = {
                        .id = cmd->id & 0x1FFFFFFFU,
                        .ide = (cmd->id & 0x80000000U) ? 1 : 0,
                        .fdf = s_cfg.use_fd_frames,
                        .brs = s_cfg.use_fd_frames ? 1 : 0,
                        .esi = 0,
                        .dlc = s_cfg.use_fd_frames ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc),
                    },
                    .buffer = cmd->data,
                    .buffer_len = payload_len,
                };

                twai_mgr_inst_t *target_node = (cmd->node_id == 1 || cmd->node_id == 0) ? s_cfg.node1 : s_cfg.node2;
                if (target_node && !target_node->recovery_in_progress) {
                    int retries = 0;
                    while (twai_mgr_async_transmit(target_node, &tx_frame, pdMS_TO_TICKS(20)) != ESP_OK) {
                        vTaskDelay(pdMS_TO_TICKS(2));
                        if (++retries > 50) {
                            break;
                        }
                    }
                    if (retries <= 50) {
                        if (target_node == s_cfg.node1 && s_cfg.tx_frames_node1) {
                            (*s_cfg.tx_frames_node1)++;
                        } else if (target_node == s_cfg.node2 && s_cfg.tx_frames_node2) {
                            (*s_cfg.tx_frames_node2)++;
                        }
                    }
                }
                cmd_idx = (cmd_idx + 1) % RX_BUF_COUNT;
            }
        }
        close(sock);
    }
}

esp_err_t tcp_service_start(const tcp_service_config_t *config)
{
    if (!config || !config->ringbuf || !config->node1 || !config->node2) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;
    xTaskCreatePinnedToCore(tcp_sender_task, "tcp_tx", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(tcp_receiver_task, "tcp_rx", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}