/*
 * ESP-IDF port of demo/Color (WS_Matrix RGB flow on 8x8, GPIO14).
 * Original Arduino: NEO_RGB + setBrightness(50). Do not run LEDs too
 * bright (heat / board damage). Tune BRIGHTNESS (1-255, lower = dimmer).
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "ws_matrix_lut.h"

static const char *TAG = "color_matrix";

#define RGB_GPIO        14
#define MATRIX_ROW      8
#define MATRIX_COL      8
#define RGB_COUNT       64
/* Global dimmer: 1-255. Default below original 50 to reduce heat. */
#define BRIGHTNESS      25

static uint8_t s_rgb_data1[64][3];
static uint8_t s_matrix_data[8][8];

static uint8_t scale_channel(uint8_t v)
{
    return (uint8_t)(((uint32_t)v * BRIGHTNESS + 127U) / 255U);
}

static void rgb_matrix1(led_strip_handle_t strip, int x)
{
    for (int row = 0; row < MATRIX_ROW; row++) {
        for (int col = 0; col < MATRIX_COL; col++) {
            const int idx = row * 8 + col;
            if (x < 16) {
                const int lut = idx + x * 8;
                s_rgb_data1[idx][0] = g_ws_rgb_lut[lut][0];
                s_rgb_data1[idx][1] = g_ws_rgb_lut[lut][1];
                s_rgb_data1[idx][2] = g_ws_rgb_lut[lut][2];
            } else {
                if (x + row < 24) {
                    const int lut = idx + x * 8;
                    s_rgb_data1[idx][0] = g_ws_rgb_lut[lut][0];
                    s_rgb_data1[idx][1] = g_ws_rgb_lut[lut][1];
                    s_rgb_data1[idx][2] = g_ws_rgb_lut[lut][2];
                } else {
                    const int lut = (x + row - 24) * 8 + col;
                    s_rgb_data1[idx][0] = g_ws_rgb_lut[lut][0];
                    s_rgb_data1[idx][1] = g_ws_rgb_lut[lut][1];
                    s_rgb_data1[idx][2] = g_ws_rgb_lut[lut][2];
                }
            }
        }
    }

    for (int row = 0; row < MATRIX_ROW; row++) {
        for (int col = 0; col < MATRIX_COL; col++) {
            const int i = row * 8 + col;
            if (s_matrix_data[row][col] == 1) {
                const uint8_t r = scale_channel(s_rgb_data1[i][0]);
                const uint8_t g = scale_channel(s_rgb_data1[i][1]);
                const uint8_t b = scale_channel(s_rgb_data1[i][2]);
                ESP_ERROR_CHECK(led_strip_set_pixel(strip, (uint32_t)i, r, g, b));
            } else {
                ESP_ERROR_CHECK(led_strip_set_pixel(strip, (uint32_t)i, 0, 0, 0));
            }
        }
    }
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

void app_main(void)
{
    memset(s_matrix_data, 1, sizeof(s_matrix_data));

    led_strip_handle_t strip = NULL;
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = RGB_COUNT,
        .led_model = LED_MODEL_WS2812,
        /* Arduino sketch used NEO_RGB (R,G,B order). If colors look swapped, try LED_STRIP_COLOR_COMPONENT_FMT_GRB. */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = true},
    };

    ESP_LOGI(TAG, "Init WS2812 strip on GPIO%d", RGB_GPIO);
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));

    int x = 0;
    while (1) {
        rgb_matrix1(strip, x);
        vTaskDelay(pdMS_TO_TICKS(30));
        x++;
        if (x == 24) {
            x = 0;
        }
    }
}
