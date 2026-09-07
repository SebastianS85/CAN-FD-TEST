#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "secrets.h"
#include "app_setup.h"
#include "app_peripherals.h"
#include "app_stats_ui.h"
#include "can_services.h"
#include "c_oled.h"

static const app_setup_can_config_t can_config = {
    .node1 = {
        .arbitration_bitrate = 500000,
        .data_bitrate =0,
        .mode = CAN_MODE_NORMAL,
        .listen_only =false,
    },
    .node2 = {
        .arbitration_bitrate = 1000000,
        .data_bitrate = 2000000,
        .mode = CAN_MODE_FD,
        .listen_only = false,
    },
};

// Definicje standardowych PID-ów OBD-II (Service 01)
#define OBD2_PID_COOLANT_TEMP  0x05  // Temperatura płynu chłodzącego
#define OBD2_PID_MAP_PRESSURE  0x0B  // Ciśnienie w dolocie (MAP / doładowanie)
#define OBD2_PID_ENGINE_RPM    0x0C  // Obroty silnika
#define OBD2_PID_VEHICLE_SPEED 0x0D  // Prędkość pojazdu
#define OBD2_PID_INTAKE_TEMP   0x0F  // Temperatura powietrza dolotowego
#define OBD2_PID_THROTTLE_POS  0x11  // Pozycja przepustnicy

// Uniwersalna funkcja wysyłająca zapytanie o dowolny PID
static esp_err_t send_obd_query_pid(uint8_t pid)
{
    uint8_t query_data[8] = {
        0x02,  // Liczba dodatkowych bajtów protokołu
        0x01,  // Service 01 (dane bieżące)
        pid,   // Identyfikator parametru (PID)
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC // Bajty wypełnienia (padding)
    };

    const twai_frame_t query_frame = {
        .header = {
            .id = 0x7DF, // Adres broadcast OBD-II
            .ide = 0,
            .fdf = 0,
            .brs = 0,
            .esi = 0,
            .dlc = 8,
        },
        .buffer = query_data,
        .buffer_len = sizeof(query_data),
    };

    esp_err_t err = twai_mgr_async_transmit(can_services_get_node(1), &query_frame,
                                            pdMS_TO_TICKS(50));
    if (err == ESP_OK) {
        app_stats_inc_tx_node1();
    }
    return err;
}

// Funkcja cyklicznie odpytująca kolejne parametry (do wstawienia w pętlę taska)
esp_err_t send_next_obd_query_on_can1(void)
{
    // Lista parametrów do cyklicznego odpytywania
    static const uint8_t query_pids[] = {
        OBD2_PID_ENGINE_RPM,     // RPM częściej
        OBD2_PID_VEHICLE_SPEED,  // Prędkość
        OBD2_PID_ENGINE_RPM,     // RPM ponownie dla płynności wykresu
        OBD2_PID_MAP_PRESSURE,   // Ciśnienie doładowania
        OBD2_PID_COOLANT_TEMP,   // Temperatura cieczy
        OBD2_PID_THROTTLE_POS,   // Położenie pedału/przepustnicy
    };
    static size_t pid_index = 0;

    uint8_t current_pid = query_pids[pid_index];
    pid_index = (pid_index + 1) % (sizeof(query_pids) / sizeof(query_pids[0]));

    return send_obd_query_pid(current_pid);
}

static void obd_query_task(void *pvParameters)
{
    // Oczekiwanie na pełną inicjalizację sterowników TWAI i magistrali
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Interwał zapytania: 100 ms (10 zapytań na sekundę)
    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        esp_err_t err = send_next_obd_query_on_can1();
        if (err != ESP_OK) {
            ESP_LOGW("OBD_TASK", "Błąd wysyłania PID: %s", esp_err_to_name(err));
        }
        vTaskDelayUntil(&last_wake_time, period);
    }
}


void app_main(void)
{
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_log_level_set("esp_twai", ESP_LOG_WARN);
    ESP_ERROR_CHECK(app_setup_init(WIFI_SSID, WIFI_PASS, &can_config));
   // ESP_ERROR_CHECK(c_oled_set_rotation_180(false));
    // BaseType_t task_created = xTaskCreate(obd_query_task, "obd task", 3072, NULL, 3, NULL);
    //  ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}