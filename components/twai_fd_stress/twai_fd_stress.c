#include "twai_fd_stress.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "esp_timer.h"

typedef struct {
    twai_frame_header_t header;
    uint16_t len;
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
} twai_fd_rx_item_t;

typedef struct {
    twai_node_handle_t node;
    QueueHandle_t rx_queue;
    TaskHandle_t tx_task;
    TaskHandle_t rx_task;
    uint64_t tx_ok;
    uint64_t tx_fail;
    uint64_t rx;
    uint64_t err;
    uint32_t tx_seq;
    uint8_t channel_id;
} twai_fd_channel_t;

typedef struct {
    bool started;
    bool run_tasks;
    twai_fd_stress_config_t cfg;
    twai_fd_channel_t ch[2];
    portMUX_TYPE lock;
} twai_fd_stress_ctx_t;

static const char *TAG = "twai_fd_stress";
static twai_fd_stress_ctx_t s_ctx = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static esp_err_t twai_start_compat(twai_fd_channel_t *ch)
{
    return twai_node_enable(ch->node);
}

static inline void stats_inc(uint64_t *counter)
{
    portENTER_CRITICAL(&s_ctx.lock);
    (*counter)++;
    portEXIT_CRITICAL(&s_ctx.lock);
}

static bool IRAM_ATTR on_tx_done_cb(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *user_ctx)
{
    (void)node;
    twai_fd_channel_t *ch = (twai_fd_channel_t *)user_ctx;
    if (edata->is_tx_success) {
        ch->tx_ok++;
    } else {
        ch->tx_fail++;
    }
    return false;
}

static bool IRAM_ATTR on_rx_done_cb(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)edata;
    twai_fd_channel_t *ch = (twai_fd_channel_t *)user_ctx;
    twai_fd_rx_item_t item = {0};

    twai_frame_t rx_frame = {
        .buffer = item.data,
        .buffer_len = sizeof(item.data),
    };

    if (twai_node_receive_from_isr(node, &rx_frame) == ESP_OK) {
        item.header = rx_frame.header;
        item.len = twaifd_dlc2len(rx_frame.header.dlc);

        BaseType_t high_task_wakeup = pdFALSE;
        if (ch->rx_queue != NULL) {
            (void)xQueueSendFromISR(ch->rx_queue, &item, &high_task_wakeup);
        }
        return high_task_wakeup == pdTRUE;
    }

    return false;
}

static bool IRAM_ATTR on_error_cb(twai_node_handle_t node, const twai_error_event_data_t *edata, void *user_ctx)
{
    (void)node;
    (void)edata;
    twai_fd_channel_t *ch = (twai_fd_channel_t *)user_ctx;
    ch->err++;
    return false;
}

static void rx_task(void *arg)
{
    twai_fd_channel_t *ch = (twai_fd_channel_t *)arg;
    twai_fd_rx_item_t item;

    while (s_ctx.run_tasks) {
        if (xQueueReceive(ch->rx_queue, &item, pdMS_TO_TICKS(250)) == pdTRUE) {
            stats_inc(&ch->rx);
            if (s_ctx.cfg.rx_callback != NULL) {
                twai_fd_rx_event_t event = {
                    .channel_id = ch->channel_id,
                    .header = item.header,
                    .len = item.len,
                };
                if (item.len > 0 && item.len <= sizeof(event.data)) {
                    memcpy(event.data, item.data, item.len);
                }
                s_ctx.cfg.rx_callback(&event, s_ctx.cfg.rx_callback_ctx);
            }
        }
    }

    vTaskDelete(NULL);
}

static void tx_task(void *arg)
{
    twai_fd_channel_t *ch = (twai_fd_channel_t *)arg;

    uint8_t payload[TWAIFD_FRAME_MAX_LEN] = {0};
    twai_frame_t frame = {
        .header = {
            .id = (ch->channel_id == 1) ? 0x111 : 0x222,
            .fdf = 1,
            .brs = 1,
            .dlc = twaifd_len2dlc((uint16_t)s_ctx.cfg.tx_payload_len),
        },
        .buffer = payload,
        .buffer_len = s_ctx.cfg.tx_payload_len,
    };

    while (s_ctx.run_tasks) {
        uint32_t seq = ch->tx_seq++;
        for (uint32_t i = 0; i < s_ctx.cfg.tx_payload_len; i++) {
            payload[i] = (uint8_t)(seq + i + ch->channel_id);
        }

        esp_err_t err = twai_node_transmit(ch->node, &frame, 0);
        if (err == ESP_OK) {
            stats_inc(&ch->tx_ok);
        } else {
            stats_inc(&ch->tx_fail);
        }

        // Bezpieczne opóźnienie 1 tick (1ms) eliminujące błąd asercji
        vTaskDelay(1);
    }

    vTaskDelete(NULL);
}

static esp_err_t init_one_channel(twai_fd_channel_t *ch, gpio_num_t tx_io, gpio_num_t rx_io)
{
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = tx_io,
            .rx = rx_io,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {
            .bitrate = s_ctx.cfg.arbitration_bitrate,
        },
        .data_timing = {
            .bitrate = s_ctx.cfg.data_bitrate,
        },
        .tx_queue_depth = 1024,
        .fail_retry_cnt = -1,
        .flags = {
            .enable_loopback = s_ctx.cfg.enable_loopback,
            .enable_self_test = s_ctx.cfg.enable_self_test,
        },
    };

    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_cfg, &ch->node), TAG, "twai_new_node_onchip failed");

    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done_cb,
        .on_rx_done = on_rx_done_cb,
        .on_error = on_error_cb,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(ch->node, &cbs, ch), TAG, "register callbacks failed");

    ESP_RETURN_ON_ERROR(twai_start_compat(ch), TAG, "twai start failed");
    return ESP_OK;
}

esp_err_t twai_fd_stress_start(const twai_fd_stress_config_t *config)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
#if !SOC_TWAI_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#endif
#if !SOC_TWAI_FD_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#endif
#if SOC_TWAI_CONTROLLER_NUM < 2
    return ESP_ERR_NOT_SUPPORTED;
#endif
    ESP_RETURN_ON_FALSE(!s_ctx.started, ESP_ERR_INVALID_STATE, TAG, "stress test already started");
    ESP_RETURN_ON_FALSE(config->tx_payload_len > 0 && config->tx_payload_len <= TWAIFD_FRAME_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid tx_payload_len");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_ctx.cfg = *config;
    s_ctx.run_tasks = true;

    s_ctx.ch[0].channel_id = 1;
    s_ctx.ch[1].channel_id = 2;

    s_ctx.ch[0].rx_queue = xQueueCreate(256, sizeof(twai_fd_rx_item_t));
    s_ctx.ch[1].rx_queue = xQueueCreate(256, sizeof(twai_fd_rx_item_t));
    ESP_GOTO_ON_FALSE(s_ctx.ch[0].rx_queue && s_ctx.ch[1].rx_queue, ESP_ERR_NO_MEM, err, TAG, "rx queue alloc failed");

    ESP_GOTO_ON_ERROR(init_one_channel(&s_ctx.ch[0], config->ch1_tx_io, config->ch1_rx_io), err, TAG, "channel1 init failed");
    ESP_GOTO_ON_ERROR(init_one_channel(&s_ctx.ch[1], config->ch2_tx_io, config->ch2_rx_io), err, TAG, "channel2 init failed");

    BaseType_t ok1 = pdPASS;
    BaseType_t ok2 = pdPASS;
    if (config->ch1_tx_enable) {
        ok1 = xTaskCreate(tx_task, "twai_tx_ch1", 4096, &s_ctx.ch[0], 8, &s_ctx.ch[0].tx_task);
    }
    if (config->ch2_tx_enable) {
        ok2 = xTaskCreate(tx_task, "twai_tx_ch2", 4096, &s_ctx.ch[1], 8, &s_ctx.ch[1].tx_task);
    }
    BaseType_t ok3 = xTaskCreate(rx_task, "twai_rx_ch1", 4096, &s_ctx.ch[0], 8, &s_ctx.ch[0].rx_task);
    BaseType_t ok4 = xTaskCreate(rx_task, "twai_rx_ch2", 4096, &s_ctx.ch[1], 8, &s_ctx.ch[1].rx_task);

    ESP_GOTO_ON_FALSE(ok1 == pdPASS && ok2 == pdPASS && ok3 == pdPASS && ok4 == pdPASS,
                      ESP_ERR_NO_MEM, err, TAG, "task create failed");

    s_ctx.started = true;
    ESP_LOGI(TAG, "TWAI FD paced continuous blast started.");
    return ESP_OK;

err:
    (void)twai_fd_stress_stop();
    return ret;
}

esp_err_t twai_fd_stress_stop(void)
{
    if (!s_ctx.started && !s_ctx.run_tasks) {
        return ESP_OK;
    }

    s_ctx.run_tasks = false;
    vTaskDelay(pdMS_TO_TICKS(20));

    for (int i = 0; i < 2; i++) {
        if (s_ctx.ch[i].tx_task != NULL) {
            vTaskDelete(s_ctx.ch[i].tx_task);
            s_ctx.ch[i].tx_task = NULL;
        }
        if (s_ctx.ch[i].rx_task != NULL) {
            vTaskDelete(s_ctx.ch[i].rx_task);
            s_ctx.ch[i].rx_task = NULL;
        }

        if (s_ctx.ch[i].node != NULL) {
            (void)twai_node_disable(s_ctx.ch[i].node);
            (void)twai_node_delete(s_ctx.ch[i].node);
            s_ctx.ch[i].node = NULL;
        }

        if (s_ctx.ch[i].rx_queue != NULL) {
            vQueueDelete(s_ctx.ch[i].rx_queue);
            s_ctx.ch[i].rx_queue = NULL;
        }
    }

    s_ctx.started = false;
    return ESP_OK;
}

esp_err_t twai_fd_stress_get_stats(twai_fd_stress_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(stats != NULL, ESP_ERR_INVALID_ARG, TAG, "stats is null");

    portENTER_CRITICAL(&s_ctx.lock);
    stats->ch1_tx_ok = s_ctx.ch[0].tx_ok;
    stats->ch1_tx_fail = s_ctx.ch[0].tx_fail;
    stats->ch1_rx = s_ctx.ch[0].rx;
    stats->ch1_err = s_ctx.ch[0].err;
    stats->ch2_tx_ok = s_ctx.ch[1].tx_ok;
    stats->ch2_tx_fail = s_ctx.ch[1].tx_fail;
    stats->ch2_rx = s_ctx.ch[1].rx;
    stats->ch2_err = s_ctx.ch[1].err;
    portEXIT_CRITICAL(&s_ctx.lock);

    return ESP_OK;
}

esp_err_t twai_fd_stress_channel_test(uint32_t window_ms, twai_fd_channel_test_result_t *result)
{
    twai_fd_stress_stats_t s0 = {0};
    twai_fd_stress_stats_t s1 = {0};

    ESP_RETURN_ON_FALSE(result != NULL, ESP_ERR_INVALID_ARG, TAG, "result is null");
    ESP_RETURN_ON_FALSE(s_ctx.started, ESP_ERR_INVALID_STATE, TAG, "stress test is not running");

    if (window_ms == 0) {
        window_ms = 1000;
    }

    ESP_RETURN_ON_ERROR(twai_fd_stress_get_stats(&s0), TAG, "failed to read start stats");
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    ESP_RETURN_ON_ERROR(twai_fd_stress_get_stats(&s1), TAG, "failed to read end stats");

    memset(result, 0, sizeof(*result));
    result->window_ms = window_ms;

    result->ch1_tx_ok_delta = s1.ch1_tx_ok - s0.ch1_tx_ok;
    result->ch1_tx_fail_delta = s1.ch1_tx_fail - s0.ch1_tx_fail;
    result->ch1_rx_delta = s1.ch1_rx - s0.ch1_rx;
    result->ch1_err_delta = s1.ch1_err - s0.ch1_err;

    result->ch2_tx_ok_delta = s1.ch2_tx_ok - s0.ch2_tx_ok;
    result->ch2_tx_fail_delta = s1.ch2_tx_fail - s0.ch2_tx_fail;
    result->ch2_rx_delta = s1.ch2_rx - s0.ch2_rx;
    result->ch2_err_delta = s1.ch2_err - s0.ch2_err;

    result->ch1_pass = (result->ch1_tx_ok_delta > 0) && (result->ch1_err_delta == 0);
    result->ch2_pass = (result->ch2_tx_ok_delta > 0) && (result->ch2_err_delta == 0);

    return ESP_OK;
}