#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>

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

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t node_id;
    uint32_t arbitration_bitrate;
    uint32_t data_bitrate;
    uint8_t listen_only;
} can_control_command_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint16_t payload_size;
} tcp_packet_header_t;

/* Optional CAN1->CAN2 bridge frame replacement, configurable remotely over TCP.
 * For frames matching filter_id (when filter_enabled), the entire outgoing
 * frame (ID/IDE, DLC, data) is replaced with new_id/new_dlc/new_data. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t enabled;
    uint8_t filter_enabled;
    uint32_t filter_id;
    uint32_t new_id;
    uint8_t new_dlc;
    uint8_t new_data[64];
} bridge_frame_replace_command_t;

typedef enum {
    APP_DISPLAY_MODE_BRIDGE = 0,
    APP_DISPLAY_MODE_SD_LOGGER,
    APP_DISPLAY_MODE_TCP_SERVER,
    /* Remote mode is enabled but no mode has been selected by the client yet. */
    APP_DISPLAY_MODE_NONE,
} app_display_mode_t;

#endif 
