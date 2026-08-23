#include "pcf8574.h"

#include "esp_check.h"

static esp_err_t validate_pin(uint8_t pin)
{
    return (pin < PCF8574_PIN_COUNT) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t validate_device(const pcf8574_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t pcf8574_init(pcf8574_t *dev,
                       i2c_master_bus_handle_t bus_handle,
                       uint8_t i2c_addr,
                       uint32_t scl_speed_hz,
                       int timeout_ms,
                       uint8_t initial_value)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "pcf8574", "dev is null");
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, "pcf8574", "bus_handle is null");
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, "pcf8574", "timeout_ms must be > 0");

    if (i2c_addr == 0) {
        i2c_addr = PCF8574_I2C_ADDR_DEFAULT;
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
                        "pcf8574", "failed to add I2C device");

    dev->bus_handle = bus_handle;
    dev->i2c_addr = i2c_addr;
    dev->output_latch = initial_value;
    dev->timeout_ms = timeout_ms;

    return pcf8574_write_byte(dev, initial_value);
}

esp_err_t pcf8574_deinit(pcf8574_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "pcf8574", "dev is null");
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

esp_err_t pcf8574_write_byte(pcf8574_t *dev, uint8_t value)
{
    ESP_RETURN_ON_ERROR(validate_device(dev), "pcf8574", "device not initialized");

    esp_err_t err = i2c_master_transmit(dev->dev_handle, &value, 1, dev->timeout_ms);
    if (err == ESP_OK) {
        dev->output_latch = value;
    }
    return err;
}

esp_err_t pcf8574_read_byte(pcf8574_t *dev, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, "pcf8574", "value is null");
    ESP_RETURN_ON_ERROR(validate_device(dev), "pcf8574", "device not initialized");

    return i2c_master_receive(dev->dev_handle, value, 1, dev->timeout_ms);
}

esp_err_t pcf8574_write_pin(pcf8574_t *dev, uint8_t pin, bool high)
{
    ESP_RETURN_ON_ERROR(validate_pin(pin), "pcf8574", "pin must be 0-7");

    uint8_t value = dev != NULL ? dev->output_latch : 0;
    if (high) {
        value |= (uint8_t)(1U << pin);
    } else {
        value &= (uint8_t)~(1U << pin);
    }
    return pcf8574_write_byte(dev, value);
}

esp_err_t pcf8574_read_pin(pcf8574_t *dev, uint8_t pin, bool *high)
{
    ESP_RETURN_ON_FALSE(high != NULL, ESP_ERR_INVALID_ARG, "pcf8574", "high is null");
    ESP_RETURN_ON_ERROR(validate_pin(pin), "pcf8574", "pin must be 0-7");

    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(pcf8574_read_byte(dev, &value), "pcf8574", "failed to read pin");
    *high = (value & (uint8_t)(1U << pin)) != 0;
    return ESP_OK;
}

esp_err_t pcf8574_toggle_pin(pcf8574_t *dev, uint8_t pin)
{
    ESP_RETURN_ON_ERROR(validate_pin(pin), "pcf8574", "pin must be 0-7");
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "pcf8574", "dev is null");

    return pcf8574_write_pin(dev, pin, (dev->output_latch & (uint8_t)(1U << pin)) == 0);
}
