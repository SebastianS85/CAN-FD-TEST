#include <time.h>
#include <sys/time.h>
#include "app_peripherals.h"
#include "esp_log.h"

static const char *TAG = "app_periph";
static app_peripherals_handles_t s_handles = {0};

app_peripherals_handles_t *app_peripherals_get_handles(void) {
    return &s_handles;
}

esp_err_t app_peripherals_set_datetime(uint16_t year, uint8_t month, uint8_t date,
                                       uint8_t day, uint8_t hour, uint8_t minute,
                                       uint8_t second) {
    ds3231mz_datetime_t datetime = {
        .year = year,
        .month = month,
        .date = date,
        .day = day,
        .hour = hour,
        .minute = minute,
        .second = second,
    };

    esp_err_t err = ds3231mz_set_time(&s_handles.rtc, &datetime);
    if (err != ESP_OK) {
        return err;
    }
    return sync_system_time_from_rtc(&s_handles.rtc);
}

esp_err_t sync_system_time_from_rtc(ds3231mz_t *rtc_dev) {
    ds3231mz_datetime_t dt;
    
    esp_err_t err = ds3231mz_read_time(rtc_dev, &dt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time from DS3231: %s", esp_err_to_name(err));
        return err;
    }

    struct tm tinfo = {0};
    tinfo.tm_year = dt.year - 1900; 
    tinfo.tm_mon  = dt.month - 1;   
    tinfo.tm_mday = dt.date;
    tinfo.tm_hour = dt.hour;
    tinfo.tm_min  = dt.minute;
    tinfo.tm_sec  = dt.second;
    tinfo.tm_isdst = -1;            

    time_t sys_time = mktime(&tinfo);

    struct timeval now = { .tv_sec = sys_time, .tv_usec = 0 };
    settimeofday(&now, NULL);

    ESP_LOGI(TAG, "System time synced with DS3231: %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.date, dt.hour, dt.minute, dt.second);

    return ESP_OK;
}

esp_err_t app_peripherals_init(const app_peripherals_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = config->port,
        .sda_io_num = config->sda_pin,
        .scl_io_num = config->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_handles.i2c_bus));
    
    s_handles.oled_available = false;
    if (i2c_master_probe(s_handles.i2c_bus, config->oled_addr, 50) == ESP_OK) {
        if (c_oled_init(s_handles.i2c_bus) == ESP_OK) {
            s_handles.oled_available = true;
        }
    }

    if (ds3231mz_init(&s_handles.rtc, s_handles.i2c_bus, 0, config->freq_hz, 50) == ESP_OK) {
        bool power_lost = false;
        ds3231mz_get_power_lost(&s_handles.rtc, &power_lost);
        if (power_lost) {
            ds3231mz_datetime_t def_time = { .year = 2026, .month = 8, .date = 26, .day = 3, .hour = 12, .minute = 0, .second = 0 };
            ds3231mz_set_time(&s_handles.rtc, &def_time);
            ds3231mz_clear_power_lost(&s_handles.rtc);
        }
        sync_system_time_from_rtc(&s_handles.rtc);
    }

    ESP_ERROR_CHECK(pcf8574_init(&s_handles.pcf8574, s_handles.i2c_bus, config->pcf8574_addr, config->freq_hz, 50, 0xFF));

    return ESP_OK;
}
