/*
 * Minimal QMI8658 init/read over I2C for ESP-IDF (demo/Game wiring).
 * Pins: SDA GPIO11, SCL GPIO12 (matches demo/Game WS_QMI8658.cpp).
 * Register map aligned with QST QMI8658 datasheet / RIOT driver constants.
 */
#include <stddef.h>

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "qmi8658_esp.h"

#define I2C_PORT        I2C_NUM_0
#define PIN_SDA         11
#define PIN_SCL         12

#define REG_RESET       0x60
#define REG_WHO_AM_I    0x00
#define REG_CTRL1       0x02
#define REG_CTRL2       0x03
#define REG_CTRL3       0x04
#define REG_CTRL7       0x08
#define REG_AX_L        0x35

static const char *TAG = "qmi8658_esp";
static uint8_t s_addr = 0x6B;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, s_addr, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_write_read_device(I2C_PORT, s_addr, &reg, 1, out, len, pdMS_TO_TICKS(100));
}

esp_err_t qmi8658_esp_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
        .clk_flags = 0,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &conf), TAG, "i2c_param_config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c_driver_install");

    const uint8_t try_addrs[] = {0x6B, 0x6A};
    for (size_t i = 0; i < sizeof(try_addrs); i++) {
        s_addr = try_addrs[i];
        (void)reg_write(REG_RESET, 0xB0);
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t who = 0;
    bool ok = false;
    const uint8_t addrs[] = {0x6B, 0x6A};
    for (size_t i = 0; i < sizeof(addrs); i++) {
        s_addr = addrs[i];
        if (reg_read(REG_WHO_AM_I, &who, 1) == ESP_OK && who == 0x05) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        ESP_LOGE(TAG, "WHO_AM_I got 0x%02x (expected 0x05); check I2C wiring.", who);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Found QMI8658 at I2C address 0x%02x", s_addr);

    /* CTRL1: auto-increment reads; clear SensorDisable so CTRL2/3 apply */
    uint8_t c1 = 0;
    reg_read(REG_CTRL1, &c1, 1);
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL1, (uint8_t)((c1 | 0x40) & ~0x01)), TAG, "ctrl1");

    /*
     * CTRL2: accel 250 Hz ODR, ±4 g — encoding per QMI8658 table (see RIOT qmi8658 driver).
     * CTRL3: gyro enabled at modest rate when accel runs fast — keeps chip happy near Arduino demo.
     */
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL2, 0x15), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL3, 0x56), TAG, "ctrl3");
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x03), TAG, "ctrl7 acc+gyro");

    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t qmi8658_esp_read_accel(float *ax_g, float *ay_g, float *az_g)
{
    uint8_t raw[6];
    ESP_RETURN_ON_ERROR(reg_read(REG_AX_L, raw, sizeof(raw)), TAG, "read ax");

    int16_t xi = (int16_t)(raw[0] | (raw[1] << 8));
    int16_t yi = (int16_t)(raw[2] | (raw[3] << 8));
    int16_t zi = (int16_t)(raw[4] | (raw[5] << 8));

    const float scale = 4.0f / 32768.0f;
    *ax_g = xi * scale;
    *ay_g = yi * scale;
    *az_g = zi * scale;
    return ESP_OK;
}
