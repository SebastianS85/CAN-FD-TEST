#include <errno.h>
#include <string.h>
#include "tcp_service.h"
#include "app_types.h"
#include "app_mode_state.h"
#include "can_services.h"
#include "wifi_manager.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <arpa/inet.h>
#include "esp_log.h"
#include "freertos/task.h"

#define FRAMES_PER_PACKET 40
#define TCP_CAN_CONTROL_MAGIC 0x314E4143U
#define TCP_PACKET_TYPE_CAN_FRAME 1U
#define TCP_PACKET_TYPE_CAN_CONTROL 2U
#define TCP_PACKET_TYPE_MODE_CONTROL 4U
#define TCP_PACKET_TYPE_BRIDGE_ID_MANIP 5U
#define APP_MODE_QUERY_ONLY 0xFFU
/* Largest control payload the receiver has to buffer (covers udp_cmd_frame_t and bridge_frame_replace_command_t). */
#define TCP_MAX_CONTROL_PAYLOAD 96U

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

static bool receive_all(int sock, void *data, size_t size)
{
    uint8_t *buffer = data;
    size_t received = 0;
    while (received < size) {
        int len = recv(sock, buffer + received, size - received, 0);
        if (len < 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && wifi_manager_is_connected()) continue;
            return false;
        }
        if (len == 0) return false;
        received += len;
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
            if (app_mode_state_get_mode() != APP_DISPLAY_MODE_TCP_SERVER) {
                /* Do not touch the shared ring buffer outside of exclusive TCP-server
                 * mode, so SD logging/bridging keep exclusive access to it. */
                uint8_t probe;
                int peek = recv(sock, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
                if (peek == 0 || (peek < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            TickType_t wait_time = frame_count == 0 ? pdMS_TO_TICKS(100) : pdMS_TO_TICKS(50);
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

        while (1) {
            tcp_packet_header_t header;
            if (!receive_all(sock, &header, sizeof(header))) break;
            if (header.payload_size > TCP_MAX_CONTROL_PAYLOAD) {
                ESP_LOGW("TCP_RX", "Discarded oversized TCP command");
                break;
            }

            uint8_t payload[TCP_MAX_CONTROL_PAYLOAD];
            if (!receive_all(sock, payload, header.payload_size)) break;

            if (header.type == TCP_PACKET_TYPE_CAN_CONTROL &&
                header.payload_size == sizeof(can_control_command_t)) {
                can_control_command_t command;
                memcpy(&command, payload, sizeof(command));
                uint8_t response = 1;
                if (command.magic != TCP_CAN_CONTROL_MAGIC) {
                    ESP_LOGW("TCP_RX", "Discarded invalid CAN control command");
                } else {
                    esp_err_t err = can_services_reconfigure_node(command.node_id,
                                                                  command.arbitration_bitrate,
                                                                  command.data_bitrate,
                                                                  command.listen_only != 0);
                    if (err == ESP_OK) {
                        response = 0;
                        ESP_LOGI("TCP_RX", "CAN %u reconfigured to %lu/%lu bit/s (listen_only=%u)", command.node_id,
                                 (unsigned long)command.arbitration_bitrate,
                                 (unsigned long)command.data_bitrate, command.listen_only);
                    } else {
                        ESP_LOGE("TCP_RX", "CAN %u configuration failed: %s", command.node_id,
                                 esp_err_to_name(err));
                    }
                }
                if (!send_all(sock, &response, sizeof(response))) break;
                continue;
            }

            if (header.type == TCP_PACKET_TYPE_BRIDGE_ID_MANIP &&
                header.payload_size == sizeof(bridge_frame_replace_command_t)) {
                bridge_frame_replace_command_t command;
                memcpy(&command, payload, sizeof(command));
                uint8_t response = 1;
                if (command.magic != TCP_CAN_CONTROL_MAGIC) {
                    ESP_LOGW("TCP_RX", "Discarded invalid bridge frame replace command");
                } else {
                    can_services_set_bridge_frame_replace(command.enabled != 0, command.filter_enabled != 0,
                                                          command.filter_id, command.new_id, command.new_dlc,
                                                          command.new_data, sizeof(command.new_data));
                    response = 0;
                    ESP_LOGI("TCP_RX", "Bridge frame replace %s (filter=%s 0x%08lX new_id=0x%08lX dlc=%u)",
                             command.enabled ? "enabled" : "disabled",
                             command.filter_enabled ? "on" : "off",
                             (unsigned long)command.filter_id, (unsigned long)command.new_id, command.new_dlc);
                }
                if (!send_all(sock, &response, sizeof(response))) break;
                continue;
            }

            if (header.type == TCP_PACKET_TYPE_MODE_CONTROL && header.payload_size == sizeof(uint8_t)) {
                uint8_t requested = payload[0];
                uint8_t ack = 1;
                if (requested == APP_MODE_QUERY_ONLY) {
                    ack = 0;
                } else if (requested <= APP_DISPLAY_MODE_TCP_SERVER) {
                    if (app_mode_state_set_mode((app_display_mode_t)requested)) {
                        ack = 0;
                        ESP_LOGI("TCP_RX", "Remote mode switch requested: %u", requested);
                    } else {
                        ESP_LOGW("TCP_RX", "Rejected mode switch: remote control disabled");
                    }
                }
                uint8_t response[2] = {
                    ack,
                    (uint8_t)((app_mode_state_get_mode() & 0x0F) |
                              (app_mode_state_is_remote() ? 0x80 : 0)),
                };
                if (!send_all(sock, response, sizeof(response))) break;
                continue;
            }

            if (header.type == TCP_PACKET_TYPE_CAN_FRAME && header.payload_size == sizeof(udp_cmd_frame_t)) {
                udp_cmd_frame_t *cmd = (udp_cmd_frame_t *)payload;
                twai_mgr_inst_t *target_node =
                    (cmd->node_id == 1 || cmd->node_id == 0) ? s_cfg.node1 : s_cfg.node2;
                if (!target_node || target_node->recovery_in_progress) {
                    continue;
                }
                bool use_fd_frames = target_node->current_cfg.mode == TWAI_BUS_MODE_FD;
                uint8_t safe_dlc = twai_mgr_bound_dlc(cmd->dlc);
                uint8_t payload_len = twai_mgr_dlc_to_len(safe_dlc);
                if (!use_fd_frames && payload_len > 8) {
                    payload_len = 8;
                }
                twai_frame_t tx_frame = {
                    .header = {
                        .id = cmd->id & 0x1FFFFFFFU,
                        .ide = (cmd->id & 0x80000000U) ? 1 : 0,
                        .fdf = use_fd_frames,
                        .brs = use_fd_frames ? 1 : 0,
                        .esi = 0,
                        .dlc = use_fd_frames ? safe_dlc : ((safe_dlc > 8) ? 8 : safe_dlc),
                    },
                    .buffer = cmd->data,
                    .buffer_len = payload_len,
                };

                if (target_node) {
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
            } else {
                ESP_LOGW("TCP_RX", "Discarded invalid TCP command packet (type=%u, size=%u)",
                         header.type, header.payload_size);
                uint8_t response = 1;
                if (!send_all(sock, &response, sizeof(response))) break;
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