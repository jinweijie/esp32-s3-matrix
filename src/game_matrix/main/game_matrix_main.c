/*
 * ESP-IDF port of demo/Game: QMI8658 tilt → move dot on 8×8 WS2812 (GPIO 14).
 * Original: NEO_RGB, row*8+col mapping, setBrightness(60). Heat warning: keep BRIGHTNESS moderate.
 */
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "qmi8658_esp.h"

static const char *TAG = "game_matrix";

#define RGB_GPIO        14
#define MATRIX_ROW      8
#define MATRIX_COL      8
#define RGB_COUNT       64
#define BRIGHTNESS      25

static uint8_t s_matrix_data[8][8];
static uint8_t s_rgb[3] = {30, 30, 30};

static uint8_t scale_channel(uint8_t v)
{
    return (uint8_t)(((uint32_t)v * BRIGHTNESS + 127U) / 255U);
}

static void rgb_matrix(led_strip_handle_t strip)
{
    for (int row = 0; row < MATRIX_ROW; row++) {
        for (int col = 0; col < MATRIX_COL; col++) {
            const int idx = row * 8 + col;
            if (s_matrix_data[row][col] == 1) {
                ESP_ERROR_CHECK(led_strip_set_pixel(strip, (uint32_t)idx,
                                                    scale_channel(s_rgb[0]),
                                                    scale_channel(s_rgb[1]),
                                                    scale_channel(s_rgb[2])));
            } else {
                ESP_ERROR_CHECK(led_strip_set_pixel(strip, (uint32_t)idx, 0, 0, 0));
            }
        }
    }
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static int s_x = 4;
static int s_y = 4;

static void game_step(led_strip_handle_t strip, uint8_t x_en, uint8_t y_en)
{
    s_matrix_data[s_x][s_y] = 0;
    if (x_en && y_en) {
        if (x_en == 1) {
            s_x = s_x + 1;
        } else {
            s_x = s_x - 1;
        }
        if (y_en == 1) {
            s_y = s_y + 1;
        } else {
            s_y = s_y - 1;
        }
    } else if (x_en) {
        if (x_en == 1) {
            s_x = s_x + 1;
        } else {
            s_x = s_x - 1;
        }
    } else if (y_en) {
        if (y_en == 1) {
            s_y = s_y + 1;
        } else {
            s_y = s_y - 1;
        }
    }
    if (s_x < 0) {
        s_x = 0;
    }
    if (s_x == 8) {
        s_x = 7;
    }
    if (s_x > 8) {
        s_x = 0;
    }
    if (s_y < 0) {
        s_y = 0;
    }
    if (s_y == 8) {
        s_y = 7;
    }
    if (s_y > 8) {
        s_y = 0;
    }
    ESP_LOGD(TAG, "y=%d", s_y);
    s_matrix_data[s_x][s_y] = 1;
    rgb_matrix(strip);
}

void app_main(void)
{
    ESP_ERROR_CHECK(qmi8658_esp_init());

    led_strip_handle_t strip = NULL;
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = RGB_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = true},
    };

    ESP_LOGI(TAG, "Game (GPIO%d), RGB order, brightness %d/255", RGB_GPIO, BRIGHTNESS);
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));

    memset(s_matrix_data, 0, sizeof(s_matrix_data));
    s_matrix_data[s_x][s_y] = 1;
    rgb_matrix(strip);

    uint8_t x_en = 0;
    uint8_t y_en = 0;
    int time_x_a = 0;
    int time_x_b = 0;
    int time_y_a = 0;
    int time_y_b = 0;

    while (1) {
        float ax = 0, ay = 0, az = 0;
        if (qmi8658_esp_read_accel(&ax, &ay, &az) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ax > 0.15f || ax < 0.0f || ay > 0.15f || ay < 0.0f || az > -0.9f || az < -1.1f) {
            if (ax > 0.15f) {
                time_x_a = time_x_a + (int)(ax * 10.0f);
                time_x_b = 0;
            } else if (ax < 0.0f) {
                time_x_b = time_x_b + (int)(fabsf(ax) * 10.0f);
                time_x_a = 0;
            } else {
                time_x_a = 0;
                time_x_b = 0;
            }

            if (ay > 0.15f) {
                time_y_a = time_y_a + (int)(ay * 10.0f);
                time_y_b = 0;
            } else if (ay < 0.0f) {
                time_y_b = time_y_b + (int)(fabsf(ay) * 10.0f);
                time_y_a = 0;
            } else {
                time_y_a = 0;
                time_y_b = 0;
            }

            if (time_x_a >= 10) {
                x_en = 1;
                time_x_a = 0;
                time_x_b = 0;
            }
            if (time_x_b >= 10) {
                x_en = 2;
                time_x_a = 0;
                time_x_b = 0;
            }
            if (time_y_a >= 10) {
                y_en = 2;
                time_y_a = 0;
                time_y_b = 0;
            }
            if (time_y_b >= 10) {
                y_en = 1;
                time_y_a = 0;
                time_y_b = 0;
            }

            game_step(strip, x_en, y_en);
            x_en = 0;
            y_en = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
