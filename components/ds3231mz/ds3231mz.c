#include "ds3231mz.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"

#define DS3231MZ_REG_SECONDS 0x00
#define DS3231MZ_REG_MINUTES 0x01
#define DS3231MZ_REG_HOURS 0x02
#define DS3231MZ_REG_DAY 0x03
#define DS3231MZ_REG_DATE 0x04
#define DS3231MZ_REG_MONTH 0x05
#define DS3231MZ_REG_YEAR 0x06
#define DS3231MZ_REG_STATUS 0x0F
#define DS3231MZ_REG_TEMP_MSB 0x11

#define DS3231MZ_STATUS_OSF_MASK BIT(7)
#define DS3231MZ_SECONDS_CH_MASK BIT(7)
#define DS3231MZ_MONTH_CENTURY_MASK BIT(7)
#define DS3231MZ_HOUR_12H_MODE_MASK BIT(6)

static uint8_t dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)((((value >> 4U) & 0x0FU) * 10U) + (value & 0x0FU));
}

static esp_err_t validate_datetime(const ds3231mz_datetime_t *dt)
{
    ESP_RETURN_ON_FALSE(dt != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "dt is null");
    ESP_RETURN_ON_FALSE(dt->year >= 2000 && dt->year <= 2199, ESP_ERR_INVALID_ARG, "ds3231mz", "year out of range");
    ESP_RETURN_ON_FALSE(dt->month >= 1 && dt->month <= 12, ESP_ERR_INVALID_ARG, "ds3231mz", "month out of range");
    ESP_RETURN_ON_FALSE(dt->date >= 1 && dt->date <= 31, ESP_ERR_INVALID_ARG, "ds3231mz", "date out of range");
    ESP_RETURN_ON_FALSE(dt->day >= 1 && dt->day <= 7, ESP_ERR_INVALID_ARG, "ds3231mz", "day out of range");
    ESP_RETURN_ON_FALSE(dt->hour <= 23, ESP_ERR_INVALID_ARG, "ds3231mz", "hour out of range");
    ESP_RETURN_ON_FALSE(dt->minute <= 59, ESP_ERR_INVALID_ARG, "ds3231mz", "minute out of range");
    ESP_RETURN_ON_FALSE(dt->second <= 59, ESP_ERR_INVALID_ARG, "ds3231mz", "second out of range");
    return ESP_OK;
}

static esp_err_t ds3231mz_read_regs(ds3231mz_t *dev, uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "dev is null");
    ESP_RETURN_ON_FALSE(dev->dev_handle != NULL, ESP_ERR_INVALID_STATE, "ds3231mz", "device not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "data is null");

    uint8_t reg_addr = reg;
    return i2c_master_transmit_receive(dev->dev_handle, &reg_addr, 1, data, len, dev->timeout_ms);
}

static esp_err_t ds3231mz_write_regs(ds3231mz_t *dev, uint8_t reg, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "dev is null");
    ESP_RETURN_ON_FALSE(dev->dev_handle != NULL, ESP_ERR_INVALID_STATE, "ds3231mz", "device not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "data is null");

    ESP_RETURN_ON_FALSE(len <= 16, ESP_ERR_INVALID_ARG, "ds3231mz", "write length too large");

    uint8_t tx_buf[17] = {0};
    tx_buf[0] = reg;
    memcpy(&tx_buf[1], data, len);

    return i2c_master_transmit(dev->dev_handle, tx_buf, len + 1, dev->timeout_ms);
}

esp_err_t ds3231mz_init(ds3231mz_t *dev,
                        i2c_master_bus_handle_t bus_handle,
                        uint8_t i2c_addr,
                        uint32_t scl_speed_hz,
                        int timeout_ms)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "dev is null");
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "bus_handle is null");
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, "ds3231mz", "timeout_ms must be > 0");

    if (i2c_addr == 0) {
        i2c_addr = DS3231MZ_I2C_ADDR_DEFAULT;
    }

    if (scl_speed_hz == 0) {
        scl_speed_hz = 100000;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = scl_speed_hz,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->dev_handle),
                        "ds3231mz", "failed to add I2C device");

    dev->bus_handle = bus_handle;
    dev->i2c_addr = i2c_addr;
    dev->scl_speed_hz = scl_speed_hz;
    dev->timeout_ms = timeout_ms;

    return ESP_OK;
}

esp_err_t ds3231mz_deinit(ds3231mz_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "dev is null");

    if (dev->dev_handle == NULL) {
        return ESP_OK;
    }

    esp_err_t err = i2c_master_bus_rm_device(dev->dev_handle);
    if (err != ESP_OK) {
        return err;
    }

    dev->dev_handle = NULL;
    dev->bus_handle = NULL;
    return ESP_OK;
}

esp_err_t ds3231mz_read_time(ds3231mz_t *dev, ds3231mz_datetime_t *dt)
{
    ESP_RETURN_ON_FALSE(dt != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "dt is null");

    uint8_t raw[7] = {0};
    ESP_RETURN_ON_ERROR(ds3231mz_read_regs(dev, DS3231MZ_REG_SECONDS, raw, sizeof(raw)),
                        "ds3231mz", "failed to read time registers");

    dt->second = bcd_to_dec(raw[0] & 0x7FU);
    dt->minute = bcd_to_dec(raw[1] & 0x7FU);

    ESP_RETURN_ON_FALSE((raw[2] & DS3231MZ_HOUR_12H_MODE_MASK) == 0,
                        ESP_ERR_NOT_SUPPORTED, "ds3231mz", "12-hour mode not supported");
    dt->hour = bcd_to_dec(raw[2] & 0x3FU);

    dt->day = bcd_to_dec(raw[3] & 0x07U);
    dt->date = bcd_to_dec(raw[4] & 0x3FU);

    uint8_t month_bcd = raw[5] & 0x1FU;
    bool century = (raw[5] & DS3231MZ_MONTH_CENTURY_MASK) != 0;
    dt->month = bcd_to_dec(month_bcd);

    uint16_t base_year = century ? 2100U : 2000U;
    dt->year = (uint16_t)(base_year + bcd_to_dec(raw[6]));

    return validate_datetime(dt);
}

esp_err_t ds3231mz_set_time(ds3231mz_t *dev, const ds3231mz_datetime_t *dt)
{
    ESP_RETURN_ON_ERROR(validate_datetime(dt), "ds3231mz", "invalid datetime");

    uint16_t year_offset;
    bool century;
    if (dt->year >= 2100U) {
        century = true;
        year_offset = (uint16_t)(dt->year - 2100U);
    } else {
        century = false;
        year_offset = (uint16_t)(dt->year - 2000U);
    }

    uint8_t raw[7] = {0};
    raw[0] = dec_to_bcd(dt->second) & 0x7FU;
    raw[1] = dec_to_bcd(dt->minute) & 0x7FU;
    raw[2] = dec_to_bcd(dt->hour) & 0x3FU;
    raw[3] = dec_to_bcd(dt->day) & 0x07U;
    raw[4] = dec_to_bcd(dt->date) & 0x3FU;
    raw[5] = (uint8_t)(dec_to_bcd(dt->month) & 0x1FU);
    if (century) {
        raw[5] |= DS3231MZ_MONTH_CENTURY_MASK;
    }
    raw[6] = dec_to_bcd((uint8_t)year_offset);

    ESP_RETURN_ON_ERROR(ds3231mz_write_regs(dev, DS3231MZ_REG_SECONDS, raw, sizeof(raw)),
                        "ds3231mz", "failed to write time registers");

    return ESP_OK;
}

esp_err_t ds3231mz_read_temperature(ds3231mz_t *dev, float *temp_c)
{
    ESP_RETURN_ON_FALSE(temp_c != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "temp_c is null");

    uint8_t raw[2] = {0};
    ESP_RETURN_ON_ERROR(ds3231mz_read_regs(dev, DS3231MZ_REG_TEMP_MSB, raw, sizeof(raw)),
                        "ds3231mz", "failed to read temperature registers");

    int8_t msb = (int8_t)raw[0];
    uint8_t lsb = (raw[1] >> 6) & 0x03U;
    *temp_c = (float)msb + (0.25f * (float)lsb);

    return ESP_OK;
}

esp_err_t ds3231mz_get_power_lost(ds3231mz_t *dev, bool *power_lost)
{
    ESP_RETURN_ON_FALSE(power_lost != NULL, ESP_ERR_INVALID_ARG, "ds3231mz", "power_lost is null");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(ds3231mz_read_regs(dev, DS3231MZ_REG_STATUS, &status, 1),
                        "ds3231mz", "failed to read status register");

    *power_lost = (status & DS3231MZ_STATUS_OSF_MASK) != 0;
    return ESP_OK;
}

esp_err_t ds3231mz_clear_power_lost(ds3231mz_t *dev)
{
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(ds3231mz_read_regs(dev, DS3231MZ_REG_STATUS, &status, 1),
                        "ds3231mz", "failed to read status register");

    status = (uint8_t)(status & (~DS3231MZ_STATUS_OSF_MASK));

    ESP_RETURN_ON_ERROR(ds3231mz_write_regs(dev, DS3231MZ_REG_STATUS, &status, 1),
                        "ds3231mz", "failed to write status register");

    return ESP_OK;
}
