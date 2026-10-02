#include "io_expander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define EXIO_ADDR       0x24
#define REG_MODE        0x02
#define REG_OUTPUT      0x03
#define REG_INPUT       0x04
#define REG_PWM         0x05
#define REG_ADC         0x06
#define I2C_TIMEOUT_MS  100

// Power-on output levels: everything high except EXIO5 (USB_SEL), which must be low to route the
// native USB pins (GPIO19/20) to the Type-C "USB" port instead of the CAN transceiver. The
// console and the DIAG line live on that port. The expander keeps its outputs across ESP32
// resets, so this also undoes the CAN routing left by older firmware.
#define OUT_DEFAULT     (0xFF & ~(1u << EXIO_USB_CAN))

static i2c_master_dev_handle_t s_dev;
static uint8_t s_out = OUT_DEFAULT;
static uint8_t s_pwm_reg;
// The cached output byte is read-modify-written from several tasks (backlight, the diag
// watchdog, SD chip select, ...), so every access holds this.
static SemaphoreHandle_t s_lock;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t io_expander_init(i2c_master_bus_handle_t bus) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXIO_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    err = write_reg(REG_MODE, 0xFF); // all pins outputs
    if (err != ESP_OK) return err;
    s_out = OUT_DEFAULT;
    return write_reg(REG_OUTPUT, s_out);
}

esp_err_t io_expander_write(uint8_t pin, uint8_t level) {
    lock();
    if (level) s_out |= (1u << pin);
    else       s_out &= ~(1u << pin);
    esp_err_t err = write_reg(REG_OUTPUT, s_out);
    unlock();
    return err;
}

esp_err_t io_expander_set_backlight(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t duty = 100 - pct; // inverted on this board, per Waveshare's slider demo
    if (duty > 97) duty = 97; // Waveshare clamps here; 100% duty blanks the panel
    lock();
    s_pwm_reg = (uint8_t)(duty * 255 / 100);
    esp_err_t err = write_reg(REG_PWM, s_pwm_reg);
    unlock();
    return err;
}

esp_err_t io_expander_read_adc(uint16_t *out) {
    uint8_t cmd = REG_ADC;
    uint8_t rx[2] = { 0 };
    lock();
    esp_err_t err = i2c_master_transmit_receive(s_dev, &cmd, 1, rx, 2, I2C_TIMEOUT_MS);
    unlock();
    if (err == ESP_OK) *out = (uint16_t)(rx[1] << 8 | rx[0]);
    return err;
}

esp_err_t io_expander_read_pins(uint8_t *out) {
    uint8_t cmd = REG_INPUT;
    lock();
    esp_err_t err = i2c_master_transmit_receive(s_dev, &cmd, 1, out, 1, I2C_TIMEOUT_MS);
    unlock();
    return err;
}

uint8_t io_expander_expected(void) {
    lock();
    uint8_t v = s_out;
    unlock();
    return v;
}

esp_err_t io_expander_reassert(void) {
    // Rewrites the cached levels, so an output in use (e.g. SD chip select held low) gets the
    // same level again rather than a glitch.
    lock();
    esp_err_t err = write_reg(REG_MODE, 0xFF);
    if (err == ESP_OK) err = write_reg(REG_OUTPUT, s_out);
    if (err == ESP_OK) err = write_reg(REG_PWM, s_pwm_reg);
    unlock();
    return err;
}
