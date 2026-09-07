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

typedef enum {
    APP_DISPLAY_MODE_BRIDGE = 0,
    APP_DISPLAY_MODE_SD_LOGGER,
    APP_DISPLAY_MODE_TCP_SERVER,
    /* Remote mode is enabled but no mode has been selected by the client yet. */
    APP_DISPLAY_MODE_NONE,
} app_display_mode_t;

#endif 
