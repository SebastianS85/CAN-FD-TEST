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

#endif // APP_TYPES_H
