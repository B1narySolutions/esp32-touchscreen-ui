#include "io_expander.h"

#define EXIO_ADDR       0x24
#define REG_MODE        0x02
#define REG_OUTPUT      0x03
#define REG_PWM         0x05
#define REG_ADC         0x06
#define I2C_TIMEOUT_MS  100

static i2c_master_dev_handle_t s_dev;
static uint8_t s_out = 0xFF;

static esp_err_t write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t io_expander_init(i2c_master_bus_handle_t bus) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXIO_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    err = write_reg(REG_MODE, 0xFF); // all pins outputs
    if (err != ESP_OK) return err;
    s_out = 0xFF;
    return write_reg(REG_OUTPUT, s_out);
}

esp_err_t io_expander_write(uint8_t pin, uint8_t level) {
    if (level) s_out |= (1u << pin);
    else       s_out &= ~(1u << pin);
    return write_reg(REG_OUTPUT, s_out);
}

esp_err_t io_expander_set_backlight(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t duty = 100 - pct; // inverted on this board, per Waveshare's slider demo
    if (duty > 97) duty = 97; // Waveshare clamps here; 100% duty blanks the panel
    return write_reg(REG_PWM, (uint8_t)(duty * 255 / 100));
}

esp_err_t io_expander_read_adc(uint16_t *out) {
    uint8_t cmd = REG_ADC;
    uint8_t rx[2] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_dev, &cmd, 1, rx, 2, I2C_TIMEOUT_MS);
    if (err == ESP_OK) *out = (uint16_t)(rx[1] << 8 | rx[0]);
    return err;
}
