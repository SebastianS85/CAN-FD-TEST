#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_twai.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t channel_id;
    twai_frame_header_t header;
    uint16_t len;
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
} twai_fd_rx_event_t;

typedef void (*twai_fd_rx_callback_t)(const twai_fd_rx_event_t *event, void *user_ctx);

typedef struct {
    gpio_num_t ch1_tx_io;
    gpio_num_t ch1_rx_io;
    gpio_num_t ch2_tx_io;
    gpio_num_t ch2_rx_io;
    bool enable_ch2;
    bool ch1_tx_enable;
    bool ch2_tx_enable;
    uint32_t arbitration_bitrate;
    uint32_t data_bitrate;
    uint32_t tx_payload_len;
    uint32_t tx_frames_per_burst;
    uint32_t tx_burst_period_ms;
    uint32_t tx_interval_ms;
    uint32_t tx_queue_depth;
    bool enable_loopback;
    bool enable_self_test;
    twai_fd_rx_callback_t rx_callback;
    void *rx_callback_ctx;
} twai_fd_stress_config_t;

typedef struct {
    uint64_t ch1_tx_ok;
    uint64_t ch1_tx_fail;
    uint64_t ch1_rx;
    uint64_t ch1_err;
    uint64_t ch2_tx_ok;
    uint64_t ch2_tx_fail;
    uint64_t ch2_rx;
    uint64_t ch2_err;
} twai_fd_stress_stats_t;

typedef struct {
    uint32_t window_ms;
    uint64_t ch1_tx_ok_delta;
    uint64_t ch1_tx_fail_delta;
    uint64_t ch1_rx_delta;
    uint64_t ch1_err_delta;
    uint64_t ch2_tx_ok_delta;
    uint64_t ch2_tx_fail_delta;
    uint64_t ch2_rx_delta;
    uint64_t ch2_err_delta;
    bool ch1_pass;
    bool ch2_pass;
} twai_fd_channel_test_result_t;

#define TWAI_FD_STRESS_CONFIG_DEFAULT() {            \
    .ch1_tx_io = GPIO_NUM_4,                         \
    .ch1_rx_io = GPIO_NUM_5,                         \
    .ch2_tx_io = GPIO_NUM_23,                        \
    .ch2_rx_io = GPIO_NUM_24,                        \
    .enable_ch2 = true,                              \
    .ch1_tx_enable = true,                           \
    .ch2_tx_enable = true,                           \
    .arbitration_bitrate = 500000,                   \
    .data_bitrate = 2000000,                         \
    .tx_payload_len = 64,                            \
    .tx_frames_per_burst = 10,                       \
    .tx_burst_period_ms = 1000,                      \
    .tx_interval_ms = 10,                            \
    .tx_queue_depth = 32,                            \
    .enable_loopback = false,                        \
    .enable_self_test = false,                       \
    .rx_callback = NULL,                             \
    .rx_callback_ctx = NULL,                         \
}

esp_err_t twai_fd_stress_start(const twai_fd_stress_config_t *config);
esp_err_t twai_fd_stress_stop(void);
esp_err_t twai_fd_stress_get_stats(twai_fd_stress_stats_t *stats);
esp_err_t twai_fd_stress_channel_test(uint32_t window_ms, twai_fd_channel_test_result_t *result);

#ifdef __cplusplus
}
#endif
