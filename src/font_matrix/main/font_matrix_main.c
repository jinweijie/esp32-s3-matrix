/*
 * ESP-IDF port of demo/Font: scrolling text on 8x8 WS2812 (GPIO 14).
 * Matches Adafruit NeoMatrix layout: TOP + RIGHT + COLUMNS + PROGRESSIVE, NEO_GRB.
 * Keep brightness moderate (heat). Tune BRIGHTNESS (1-255, lower = dimmer).
 * Font: Adafruit_GFX classic glcdfont (see glcdfont.c).
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "glcdfont.h"

static const char *TAG = "font_matrix";

#define RGB_GPIO        14
#define MATRIX_W        8
#define MATRIX_H        8
#define LED_COUNT       64
/* Global dimmer: 1-255. Default below demo/Font setBrightness(40) to reduce heat. */
#define BRIGHTNESS      25

/* Logical (x,y) with x=0 left, y=0 top → strip index (demo/Font NeoMatrix flags). */
static inline int neomatrix_index(int x, int y)
{
    return (MATRIX_W - 1 - x) * MATRIX_H + y;
}

static int get_char_width(char c)
{
    if (c == 'i' || c == 'l' || c == '!' || c == '.') {
        return 3;
    }
    return 5;
}

static int get_string_width(const char *str)
{
    int width = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        width += get_char_width(str[i]);
        width += 1;
    }
    return width;
}

/* Classic 5x7 glyph; Adafruit_GFX drawChar (opaque background on last column). */
static void draw_char_at(uint8_t px[LED_COUNT][3], int16_t x0, int16_t y0, unsigned char c,
                         uint8_t fr, uint8_t fg, uint8_t fb,
                         uint8_t br, uint8_t bg, uint8_t bb)
{
    if (c == 0) {
        c = ' ';
    }

    for (int8_t i = 0; i < 5; i++) {
        uint8_t line = gfx_font5x7[(size_t)c * GFX_FONT_BYTES_PER_CHAR + (size_t)i];
        for (int8_t j = 0; j < 8; j++, line >>= 1) {
            int16_t x = x0 + i;
            int16_t y = y0 + j;
            if (x < 0 || y < 0 || x >= MATRIX_W || y >= MATRIX_H) {
                continue;
            }
            const int idx = neomatrix_index(x, y);
            if (line & 1) {
                px[idx][0] = fr;
                px[idx][1] = fg;
                px[idx][2] = fb;
            } else {
                px[idx][0] = br;
                px[idx][1] = bg;
                px[idx][2] = bb;
            }
        }
    }
    for (int8_t j = 0; j < 8; j++) {
        int16_t x = x0 + 5;
        int16_t y = y0 + j;
        if (x >= 0 && x < MATRIX_W && y >= 0 && y < MATRIX_H) {
            const int idx = neomatrix_index(x, y);
            px[idx][0] = br;
            px[idx][1] = bg;
            px[idx][2] = bb;
        }
    }
}

static void draw_string_at(uint8_t px[LED_COUNT][3], int16_t x0, int16_t y0, const char *str,
                           uint8_t fr, uint8_t fg, uint8_t fb)
{
    const uint8_t br = 0, bg = 0, bb = 0;
    int16_t x = x0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        draw_char_at(px, x, y0, (unsigned char)str[i], fr, fg, fb, br, bg, bb);
        x += get_char_width(str[i]) + 1;
    }
}

static uint8_t scale_bright(uint8_t v)
{
    return (uint8_t)(((uint32_t)v * BRIGHTNESS + 127U) / 255U);
}

static void push_frame(led_strip_handle_t strip, const uint8_t px[LED_COUNT][3])
{
    for (int i = 0; i < LED_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(strip, (uint32_t)i,
                                            scale_bright(px[i][0]),
                                            scale_bright(px[i][1]),
                                            scale_bright(px[i][2])));
    }
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

void app_main(void)
{
    static const char k_text[] = "ESP32-S3-Matrix Text Testing!";
    static int s_scroll_x = MATRIX_W;

    led_strip_handle_t strip = NULL;
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = true},
    };

    ESP_LOGI(TAG, "Font scroll (GPIO%d), GRB, brightness %d/255", RGB_GPIO, BRIGHTNESS);
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));

    const int text_w = get_string_width(k_text);
    uint8_t px[LED_COUNT][3];

    while (1) {
        memset(px, 0, sizeof(px));
        draw_string_at(px, s_scroll_x, 0, k_text, 255, 0, 0);
        push_frame(strip, px);
        vTaskDelay(pdMS_TO_TICKS(100));
        s_scroll_x--;
        if (s_scroll_x < -text_w) {
            s_scroll_x = MATRIX_W;
        }
    }
}
