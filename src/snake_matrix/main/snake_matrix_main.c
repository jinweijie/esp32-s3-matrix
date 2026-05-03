/*
 * Snake on 8×8 WS2812 — tilt to steer via QMI8658 (same wiring as idf/game_matrix).
 * GPIO 14 LEDs, NEO_RGB-style channel order. Keep BRIGHTNESS moderate (board heat).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "qmi8658_esp.h"
#include "glcdfont.h"

static const char *TAG = "snake_matrix";

#define RGB_GPIO        14
#define GRID            8
#define LED_COUNT       64
#define SNAKE_MAX       64
/* Lower than other IDF demos — full-matrix Snake can feel very bright on 8×8. */
#define BRIGHTNESS      10

#define IMU_POLL_MS     10
#define MOVE_MS         320
/** Foods eaten (score) shown after game over / win; snake starts at length 3. */
#define SCORE_DISPLAY_MS 3000

#define TILT_DEAD       0.18f
#define TILT_STRONG     0.22f

/*
 * Tilt → screen (x = right, y = down on the LED grid).
 * On ESP32-S3-Matrix the QMI8658 axes are rotated vs the panel: a physical “tilt left”
 * often shows up mostly on chip Y, which we previously fed into vertical — felt like “up”.
 * SWAP maps sensor Y → screen horizontal and sensor X → screen vertical.
 * MIRROR_Y: after swap, chip X vs “tilt up/down” on the panel is often opposite;
 * set to 1 if tilting the front **down** makes the snake go **up** (default on ESP32-S3-Matrix).
 * MIRROR_X: flips **left / right** steering (default 1 for this board).
 */
#define SNAKE_TILT_SWAP_AX_AY   1
#define SNAKE_TILT_MIRROR_X     1
#define SNAKE_TILT_MIRROR_Y     1

typedef struct {
    int8_t x;
    int8_t y;
} Pt;

static Pt s_snake[SNAKE_MAX];
static int s_len;
static int8_t s_dir_dx;
static int8_t s_dir_dy;
static Pt s_food;

static led_strip_handle_t s_strip;

static uint8_t scale(uint8_t v)
{
    return (uint8_t)(((uint32_t)v * BRIGHTNESS + 127U) / 255U);
}

static void clear_strip(void)
{
    for (int i = 0; i < LED_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, (uint32_t)i, 0, 0, 0));
    }
}

static void set_cell_xy(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    const int idx = (int)y * GRID + x;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, (uint32_t)idx, scale(r), scale(g), scale(b)));
}

static void draw_frame(void)
{
    clear_strip();
    for (int i = 0; i < s_len; i++) {
        uint8_t gr = (i == 0) ? 220 : 90;
        set_cell_xy(s_snake[i].x, s_snake[i].y, 0, gr, 0);
    }
    set_cell_xy(s_food.x, s_food.y, 255, 0, 0);
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
}

/* Strip index: x = column left→right, y = row top→bottom (same as set_cell_xy). */
static inline int snake_pix_idx(int x, int y)
{
    return y * GRID + x;
}

static int get_char_width_snake(char c)
{
    if (c == 'i' || c == 'l' || c == '!' || c == '.') {
        return 3;
    }
    return 5;
}

static int get_string_width_tight(const char *str)
{
    int width = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        width += get_char_width_snake(str[i]);
    }
    return width;
}

static void draw_char_at_px(uint8_t px[LED_COUNT][3], int16_t x0, int16_t y0, unsigned char c,
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
            if (x < 0 || y < 0 || x >= GRID || y >= GRID) {
                continue;
            }
            const int idx = snake_pix_idx(x, y);
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
        if (x >= 0 && x < GRID && y >= 0 && y < GRID) {
            const int idx = snake_pix_idx(x, y);
            px[idx][0] = br;
            px[idx][1] = bg;
            px[idx][2] = bb;
        }
    }
}

static void draw_string_tight(uint8_t px[LED_COUNT][3], int16_t x0, int16_t y0, const char *str,
                              uint8_t fr, uint8_t fg, uint8_t fb)
{
    const uint8_t br = 0, bg = 0, bb = 0;
    int16_t x = x0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        draw_char_at_px(px, x, y0, (unsigned char)str[i], fr, fg, fb, br, bg, bb);
        x += get_char_width_snake(str[i]);
    }
}

static void push_frame_px(const uint8_t px[LED_COUNT][3])
{
    for (int i = 0; i < LED_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, (uint32_t)i,
                                            scale(px[i][0]), scale(px[i][1]), scale(px[i][2])));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
}

/** Score = number of foods eaten (length − 3). */
static void show_score_display(int score)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", score);

    uint8_t px[LED_COUNT][3];
    memset(px, 0, sizeof(px));

    const int w = get_string_width_tight(buf);
    const int16_t x0 = (int16_t)((GRID - w) / 2);
    const int16_t y0 = 0;
    /* Amber digits; readable on low BRIGHTNESS. */
    draw_string_tight(px, x0, y0, buf, 255, 180, 0);

    push_frame_px(px);
    ESP_LOGI(TAG, "Score %d (shown %d ms)", score, SCORE_DISPLAY_MS);
    vTaskDelay(pdMS_TO_TICKS(SCORE_DISPLAY_MS));
}

static bool cell_on_snake(int x, int y)
{
    for (int i = 0; i < s_len; i++) {
        if (s_snake[i].x == x && s_snake[i].y == y) {
            return true;
        }
    }
    return false;
}

static void spawn_food(void)
{
    for (int n = 0; n < 300; n++) {
        int fx = (int)(esp_random() % (uint32_t)GRID);
        int fy = (int)(esp_random() % (uint32_t)GRID);
        if (!cell_on_snake(fx, fy)) {
            s_food.x = (int8_t)fx;
            s_food.y = (int8_t)fy;
            return;
        }
    }
    s_food.x = 0;
    s_food.y = 0;
}

static void game_reset(void)
{
    s_len = 3;
    s_snake[0].x = 4;
    s_snake[0].y = 4;
    s_snake[1].x = 3;
    s_snake[1].y = 4;
    s_snake[2].x = 2;
    s_snake[2].y = 4;
    s_dir_dx = 1;
    s_dir_dy = 0;
    spawn_food();
    draw_frame();
    ESP_LOGI(TAG, "Snake reset — tilt board to steer");
}

/** Map accel to desired direction; forbid instant 180° turn. */
static void update_dir_from_tilt(float ax, float ay)
{
    float sx = ax;
    float sy = ay;
#if SNAKE_TILT_SWAP_AX_AY
    const float t = sx;
    sx = sy;
    sy = t;
#endif
#if SNAKE_TILT_MIRROR_X
    sx = -sx;
#endif
#if SNAKE_TILT_MIRROR_Y
    sy = -sy;
#endif

    if (fabsf(sx) < TILT_DEAD && fabsf(sy) < TILT_DEAD) {
        return;
    }

    int8_t ndx = 0;
    int8_t ndy = 0;

    if (fabsf(sx) >= fabsf(sy)) {
        if (sx > TILT_STRONG) {
            ndx = 1;
        } else if (sx < -TILT_STRONG) {
            ndx = -1;
        } else {
            return;
        }
    } else {
        /*
         * Same as demo/Game Game.ino + WS_Matrix Game(): Accel.y > 0 → Y_EN=2 → y--;
         * Accel.y < 0 → Y_EN=1 → y++. So +sy on sensor → move up (−dy).
         */
        if (sy > TILT_STRONG) {
            ndy = -1;
        } else if (sy < -TILT_STRONG) {
            ndy = 1;
        } else {
            return;
        }
    }

    if (ndx == -s_dir_dx && ndy == -s_dir_dy) {
        return;
    }
    if (ndx != 0 && ndy != 0) {
        return;
    }

    s_dir_dx = ndx;
    s_dir_dy = ndy;
}

static void snake_tick(void)
{
    Pt nh;
    nh.x = (int8_t)(s_snake[0].x + s_dir_dx);
    nh.y = (int8_t)(s_snake[0].y + s_dir_dy);

    if (nh.x < 0 || nh.x >= GRID || nh.y < 0 || nh.y >= GRID) {
        ESP_LOGW(TAG, "Hit wall");
        show_score_display(s_len - 3);
        game_reset();
        return;
    }

    for (int i = 0; i < s_len - 1; i++) {
        if (s_snake[i].x == nh.x && s_snake[i].y == nh.y) {
            ESP_LOGW(TAG, "Hit self");
            show_score_display(s_len - 3);
            game_reset();
            return;
        }
    }

    const bool eat = (nh.x == s_food.x && nh.y == s_food.y);

    if (eat) {
        s_len++;
        for (int i = s_len - 1; i >= 1; i--) {
            s_snake[i] = s_snake[i - 1];
        }
        s_snake[0] = nh;
        if (s_len >= SNAKE_MAX) {
            ESP_LOGI(TAG, "Win — filled the grid (通关)");
            show_score_display(s_len - 3);
            game_reset();
            return;
        }
        spawn_food();
    } else {
        for (int i = s_len - 1; i >= 1; i--) {
            s_snake[i] = s_snake[i - 1];
        }
        s_snake[0] = nh;
    }

    draw_frame();
}

void app_main(void)
{
    ESP_ERROR_CHECK(qmi8658_esp_init());

    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = LED_COUNT,
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

    ESP_LOGI(TAG, "Snake GPIO%d, brightness %d/255, move every %d ms", RGB_GPIO, BRIGHTNESS, MOVE_MS);
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));

    game_reset();

    int move_accum = 0;
    while (1) {
        float ax = 0, ay = 0, az = 0;
        if (qmi8658_esp_read_accel(&ax, &ay, &az) == ESP_OK) {
            update_dir_from_tilt(ax, ay);
        }

        move_accum += IMU_POLL_MS;
        if (move_accum >= MOVE_MS) {
            move_accum = 0;
            snake_tick();
        }

        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
    }
}
