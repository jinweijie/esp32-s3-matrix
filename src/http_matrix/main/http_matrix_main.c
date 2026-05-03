/*
 * ESP-IDF port of demo/HTTP: Soft AP + web UI + scrolling text on 8×8 WS2812 (GPIO 14).
 * Matrix layout matches demo/HTTP WS_Flow.cpp: TOP + LEFT + ROWS + PROGRESSIVE (NEO_GRB).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#include "glcdfont.h"
#include "cjk8x8.h"

static const char *TAG = "http_matrix";

#define AP_SSID         "ESP32-S3-Matrix"
#define AP_PASSWORD     "adminadmin"
#define AP_IP_O1        10
#define AP_IP_O2        50
#define AP_IP_O3        50
#define AP_IP_O4        1
#define RGB_GPIO        14
#define MATRIX_W        8
#define MATRIX_H        8
#define LED_COUNT       64
#define BRIGHTNESS      10
#define TEXT_MAX        512
#define CJK_CELL_W      8
#define GLYPH_GAP       1
/*
 * With CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(1) is 0 — vTaskDelay(0) spins and can
 * starve the idle task (task WDT on IDLE0). Use a full tick (10 ms) and scale the
 * scroll counter so text still advances about every 100 ms (10 × 10 ms).
 */
#define MAIN_LOOP_DELAY_MS  10
#define SPEED_DEFAULT         5
#define PERIOD_TICKS_DEFAULT  10

typedef enum {
    DISP_MARQUEE = 0,
    DISP_ONE_CHAR,
    DISP_STATIC,
    DISP_FILL,
} disp_mode_t;

/** One-character mode: optional zoom animation (8 frames per glyph). */
typedef enum {
    OC_ZOOM_NONE = 0,
    OC_ZOOM_IN,
    OC_ZOOM_OUT,
} oc_zoom_t;

/** Scrolling marquee axis and travel direction. */
typedef enum {
    MARQUEE_DIR_LEFT = 0,
    MARQUEE_DIR_RIGHT,
    MARQUEE_DIR_UP,
    MARQUEE_DIR_DOWN,
} marquee_dir_t;

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_mtx;

static char s_text[TEXT_MAX] = "ESP32-S3-Matrix 中文测试";
/** 1 = run text/animation; 0 = full-panel fill / idle (no text frames). */
static volatile int s_flow_flag = 1;
static int s_scroll_x = MATRIX_W;
static int s_scroll_y = 0;

static volatile disp_mode_t s_disp_mode = DISP_MARQUEE;
static volatile int s_speed = SPEED_DEFAULT; /* 1 = slow … 10 = fast */
static volatile int s_period_ticks = PERIOD_TICKS_DEFAULT;
static volatile uint8_t s_cr = 255;
static volatile uint8_t s_cg = 0;
static volatile uint8_t s_cb = 0;
static volatile bool s_rainbow = false;
static volatile uint8_t s_rainbow_hue = 0;
static volatile size_t s_cc_off = 0;

static volatile marquee_dir_t s_marquee_dir = MARQUEE_DIR_LEFT;

static volatile oc_zoom_t s_oc_zoom = OC_ZOOM_NONE;
static volatile int s_oc_phase = 0;

static inline int neomatrix_index_tl_rp(int x, int y)
{
    return y * MATRIX_W + x;
}

static int get_char_width(char c)
{
    if (c == 'i' || c == 'l' || c == '!' || c == '.') {
        return 3;
    }
    return 5;
}

/** Logical width in pixels for one Unicode codepoint (plus trailing gap). */
static int width_one_codepoint(uint32_t cp)
{
    if (cp == 0x3000) {
        /* Fullwidth space (IDEOGRAPHIC SPACE). */
        return CJK_CELL_W + GLYPH_GAP;
    }
    if (cjk8_lookup(cp, NULL)) {
        return CJK_CELL_W + GLYPH_GAP;
    }
    if (cp < 32 && cp != ' ') {
        return 0;
    }
    if (cp < 256) {
        return get_char_width((char)cp) + GLYPH_GAP;
    }
    return get_char_width('?') + GLYPH_GAP;
}

static int get_string_width(const char *str)
{
    int width = 0;
    const char *p = str;
    while (*p != '\0') {
        uint32_t cp = 0;
        utf8_decode(&p, &cp);
        width += width_one_codepoint(cp);
    }
    return width;
}

static void draw_cjk8_at(uint8_t px[LED_COUNT][3], int16_t x0, int16_t y0, const uint8_t rows[8],
                         uint8_t fr, uint8_t fg, uint8_t fb,
                         uint8_t br, uint8_t bg, uint8_t bb)
{
    for (int r = 0; r < 8; r++) {
        uint8_t line = rows[r];
        for (int b = 0; b < 8; b++, line <<= 1U) {
            int16_t x = x0 + b;
            int16_t y = y0 + r;
            if (x < 0 || y < 0 || x >= MATRIX_W || y >= MATRIX_H) {
                continue;
            }
            const int idx = neomatrix_index_tl_rp(x, y);
            if (line & 0x80) {
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
}

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
            const int idx = neomatrix_index_tl_rp(x, y);
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
            const int idx = neomatrix_index_tl_rp(x, y);
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
    const char *p = str;

    while (*p != '\0') {
        uint32_t cp = 0;
        utf8_decode(&p, &cp);

        if (cp == 0x3000) {
            x += CJK_CELL_W + GLYPH_GAP;
            continue;
        }

        uint8_t cjk_rows[8];
        if (cjk8_lookup(cp, cjk_rows)) {
            draw_cjk8_at(px, x, y0, cjk_rows, fr, fg, fb, br, bg, bb);
            x += CJK_CELL_W + GLYPH_GAP;
            continue;
        }

        if (cp < 32 && cp != ' ') {
            continue;
        }

        if (cp < 256) {
            draw_char_at(px, x, y0, (unsigned char)cp, fr, fg, fb, br, bg, bb);
            x += get_char_width((char)cp) + GLYPH_GAP;
            continue;
        }

        draw_char_at(px, x, y0, '?', fr, fg, fb, br, bg, bb);
        x += get_char_width('?') + GLYPH_GAP;
    }
}

static void draw_one_codepoint(uint8_t px[LED_COUNT][3], int16_t x0, int16_t y0, uint32_t cp,
                               uint8_t fr, uint8_t fg, uint8_t fb)
{
    const uint8_t br = 0, bg = 0, bb = 0;
    if (cp == 0x3000) {
        return;
    }
    uint8_t cjk_rows[8];
    if (cjk8_lookup(cp, cjk_rows)) {
        draw_cjk8_at(px, x0, y0, cjk_rows, fr, fg, fb, br, bg, bb);
        return;
    }
    if (cp < 32 && cp != ' ') {
        return;
    }
    if (cp < 256) {
        draw_char_at(px, x0, y0, (unsigned char)cp, fr, fg, fb, br, bg, bb);
        return;
    }
    draw_char_at(px, x0, y0, '?', fr, fg, fb, br, bg, bb);
}

/**
 * Draw glyph scaled by zoom_num/zoom_den (each relative to full matrix).
 * Mask is 8×8; glyph occupies columns [0, gw) (ASCII/CJK drawn from x=0).
 */
static void blit_mask_scaled(uint8_t px[LED_COUNT][3], const uint8_t m[8][8], int gw, int zoom_num,
                             int zoom_den, uint8_t fr, uint8_t fg, uint8_t fb)
{
    if (gw < 1) {
        gw = 1;
    }
    if (zoom_num < 1) {
        zoom_num = 1;
    }
    if (zoom_den < 1) {
        zoom_den = 1;
    }

    const int scaled_w = (gw * zoom_num + zoom_den - 1) / zoom_den;
    const int scaled_h = (8 * zoom_num + zoom_den - 1) / zoom_den;
    const int ox = (MATRIX_W - scaled_w) / 2;
    const int oy = (MATRIX_H - scaled_h) / 2;

    for (int dy = 0; dy < MATRIX_H; dy++) {
        for (int dx = 0; dx < MATRIX_W; dx++) {
            if (dx < ox || dx >= ox + scaled_w || dy < oy || dy >= oy + scaled_h) {
                continue;
            }
            const int gx = ((dx - ox) * zoom_den) / zoom_num;
            const int gy = ((dy - oy) * zoom_den) / zoom_num;
            if (gx >= 0 && gx < gw && gy >= 0 && gy < 8 && m[(unsigned)gy][(unsigned)gx]) {
                const int idx = neomatrix_index_tl_rp(dx, dy);
                px[idx][0] = fr;
                px[idx][1] = fg;
                px[idx][2] = fb;
            }
        }
    }
}

/** Must be called with s_mtx held. Sets scroll start from text size and direction. */
static void marquee_reset_marquee_locked(void)
{
    const int tw = get_string_width(s_text);
    switch (s_marquee_dir) {
        case MARQUEE_DIR_LEFT:
            s_scroll_x = MATRIX_W;
            s_scroll_y = 0;
            break;
        case MARQUEE_DIR_RIGHT:
            s_scroll_x = -tw;
            s_scroll_y = 0;
            break;
        case MARQUEE_DIR_DOWN:
            s_scroll_x = 0;
            s_scroll_y = -MATRIX_H;
            break;
        case MARQUEE_DIR_UP:
            s_scroll_x = 0;
            s_scroll_y = MATRIX_H;
            break;
        default:
            s_scroll_x = MATRIX_W;
            s_scroll_y = 0;
            break;
    }
}

static void hue_to_rgb_u8(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const unsigned int h = (unsigned int)hue * 6U;
    const unsigned int sector = h >> 8;
    const unsigned int frac = h & 0xFFU;
    const unsigned int inv = 255U - frac;
    switch (sector) {
        case 0:
            *r = 255;
            *g = (uint8_t)frac;
            *b = 0;
            break;
        case 1:
            *r = (uint8_t)inv;
            *g = 255;
            *b = 0;
            break;
        case 2:
            *r = 0;
            *g = 255;
            *b = (uint8_t)frac;
            break;
        case 3:
            *r = 0;
            *g = (uint8_t)inv;
            *b = 255;
            break;
        case 4:
            *r = (uint8_t)frac;
            *g = 0;
            *b = 255;
            break;
        default:
            *r = 255;
            *g = 0;
            *b = (uint8_t)inv;
            break;
    }
}

static void apply_rainbow(uint8_t px[LED_COUNT][3], uint8_t hue_base)
{
    for (int i = 0; i < LED_COUNT; i++) {
        if (px[i][0] | px[i][1] | px[i][2]) {
            uint8_t r, g, b;
            hue_to_rgb_u8((uint8_t)(hue_base + (uint8_t)(i * 4)), &r, &g, &b);
            px[i][0] = r;
            px[i][1] = g;
            px[i][2] = b;
        }
    }
}

static uint8_t scale_bright(uint8_t v)
{
    return (uint8_t)(((uint32_t)v * BRIGHTNESS + 127U) / 255U);
}

static void push_frame(const uint8_t px[LED_COUNT][3])
{
    for (int i = 0; i < LED_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, (uint32_t)i,
                                            scale_bright(px[i][0]),
                                            scale_bright(px[i][1]),
                                            scale_bright(px[i][2])));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
}

static void color_wipe_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, (uint32_t)i,
                                            scale_bright(r), scale_bright(g), scale_bright(b)));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
}

static void display_step(void)
{
    char buf[TEXT_MAX];
    disp_mode_t mode;
    uint8_t cr, cg, cb;
    bool rainbow;
    size_t cc_off;
    int scroll_x;
    int scroll_y;
    marquee_dir_t marquee_dir;
    oc_zoom_t oc_zoom;
    int oc_phase;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(buf, s_text, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    mode = s_disp_mode;
    cr = s_cr;
    cg = s_cg;
    cb = s_cb;
    rainbow = s_rainbow;
    cc_off = s_cc_off;
    scroll_x = s_scroll_x;
    scroll_y = s_scroll_y;
    marquee_dir = s_marquee_dir;
    oc_zoom = s_oc_zoom;
    oc_phase = s_oc_phase;
    xSemaphoreGive(s_mtx);

    uint8_t px[LED_COUNT][3];
    memset(px, 0, sizeof(px));

    if (mode == DISP_FILL) {
        color_wipe_rgb(cr, cg, cb);
        return;
    }

    switch (mode) {
        case DISP_STATIC:
            draw_string_at(px, 0, 0, buf, cr, cg, cb);
            break;
        case DISP_ONE_CHAR: {
            const size_t bl = strlen(buf);
            if (bl == 0) {
                break;
            }
            size_t off = cc_off;
            if (off >= bl) {
                off = 0;
            }
            const char *p = buf + off;
            uint32_t cp = 0;
            const char *q = p;
            utf8_decode(&q, &cp);
            const size_t next_after_glyph = (size_t)(q - buf);
            size_t next_off_base = next_after_glyph;
            if (buf[next_off_base] == '\0') {
                next_off_base = 0;
            }

            int gw = width_one_codepoint(cp) - GLYPH_GAP;
            if (gw < 1) {
                gw = 1;
            }

            int zn = 8;
            const int zd = 8;
            if (oc_zoom == OC_ZOOM_IN) {
                zn = oc_phase + 1;
            } else if (oc_zoom == OC_ZOOM_OUT) {
                zn = 8 - oc_phase;
            }

            if (oc_zoom == OC_ZOOM_NONE) {
                const int w = gw;
                int x0 = (MATRIX_W - w) / 2;
                if (x0 < 0) {
                    x0 = 0;
                }
                draw_one_codepoint(px, x0, 0, cp, cr, cg, cb);
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_cc_off = next_off_base;
                s_oc_phase = 0;
                xSemaphoreGive(s_mtx);
            } else {
                uint8_t m[8][8];
                uint8_t tmp[LED_COUNT][3];
                memset(tmp, 0, sizeof(tmp));
                memset(m, 0, sizeof(m));
                draw_one_codepoint(tmp, 0, 0, cp, 1, 1, 1);
                for (int yy = 0; yy < 8; yy++) {
                    for (int xx = 0; xx < 8; xx++) {
                        const int idx = neomatrix_index_tl_rp(xx, yy);
                        if (tmp[idx][0] | tmp[idx][1] | tmp[idx][2]) {
                            m[yy][xx] = 1;
                        }
                    }
                }
                blit_mask_scaled(px, m, gw, zn, zd, cr, cg, cb);

                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_oc_phase = oc_phase + 1;
                if (s_oc_phase >= 8) {
                    s_oc_phase = 0;
                    s_cc_off = next_off_base;
                }
                xSemaphoreGive(s_mtx);
            }
            break;
        }
        case DISP_MARQUEE:
        default:
            draw_string_at(px, scroll_x, scroll_y, buf, cr, cg, cb);
            break;
    }

    if (rainbow && mode != DISP_FILL) {
        uint8_t hb;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        hb = s_rainbow_hue;
        s_rainbow_hue += 6;
        xSemaphoreGive(s_mtx);
        apply_rainbow(px, hb);
    }

    push_frame(px);

    if (mode == DISP_MARQUEE) {
        const int tw = get_string_width(buf);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (tw <= 0) {
            /* keep position */
        } else {
            switch (marquee_dir) {
                case MARQUEE_DIR_LEFT:
                    s_scroll_x--;
                    if (s_scroll_x < -tw) {
                        marquee_reset_marquee_locked();
                    }
                    break;
                case MARQUEE_DIR_RIGHT:
                    s_scroll_x++;
                    if (s_scroll_x > MATRIX_W) {
                        marquee_reset_marquee_locked();
                    }
                    break;
                case MARQUEE_DIR_DOWN:
                    s_scroll_y++;
                    if (s_scroll_y >= MATRIX_H) {
                        marquee_reset_marquee_locked();
                    }
                    break;
                case MARQUEE_DIR_UP:
                    s_scroll_y--;
                    if (s_scroll_y <= -MATRIX_H) {
                        marquee_reset_marquee_locked();
                    }
                    break;
                default:
                    break;
            }
        }
        xSemaphoreGive(s_mtx);
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-S3-Matrix</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;background:#e8eaed;margin:0;padding:24px;color:#222}"
        ".wrap{max-width:520px;margin:0 auto}"
        "header{background:#1a1a2e;color:#eee;padding:16px 20px;border-radius:12px;margin-bottom:20px}"
        "h1{margin:0;font-size:1.25rem;font-weight:600}"
        ".card{background:#fff;border-radius:12px;padding:20px;box-shadow:0 2px 12px rgba(0,0,0,.08)}"
        ".row{margin-bottom:16px}"
        ".row label{display:block;font-size:.85rem;color:#555;margin-bottom:6px;font-weight:500}"
        ".row-inline{display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
        "input[type=text]{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid #ccc;"
        "border-radius:8px;font-size:1rem}"
        "select,input[type=range]{width:100%;box-sizing:border-box}"
        ".speed-val{font-variant-numeric:tabular-nums;color:#333}"
        "button,.btn{padding:10px 16px;border:none;border-radius:8px;background:#1a1a2e;color:#fff;"
        "cursor:pointer;font-size:.95rem}"
        "button:hover,.btn:hover{background:#2d2d44}"
        "button.secondary{background:#5c6370}"
        "button.secondary:hover{background:#434953}"
        ".hint{font-size:.75rem;color:#777;margin-top:4px}"
        ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:8px}"
        "</style></head><body><div class=\"wrap\">"
        "<header><h1>ESP32-S3-Matrix</h1></header>"
        "<div class=\"card\">"
        "<div class=\"row\"><label for=\"text\">Message (UTF-8, Chinese subset)</label>"
        "<input type=\"text\" id=\"text\" maxlength=\"400\" placeholder=\"hello\" />"
        "<div class=\"actions\"><button type=\"button\" onclick=\"sendText()\">Apply text</button></div></div>"
        "<div class=\"row\"><label>Speed <span class=\"speed-val\" id=\"speedLbl\">5</span> / 10</label>"
        "<input type=\"range\" id=\"speed\" min=\"1\" max=\"10\" value=\"5\" "
        "oninput=\"document.getElementById('speedLbl').textContent=this.value\" />"
        "<p class=\"hint\">Higher number = faster scrolling or character advance.</p></div>"
        "<div class=\"row\"><label for=\"mode\">Display mode</label>"
        "<select id=\"mode\">"
        "<option value=\"marquee\">Scrolling marquee</option>"
        "<option value=\"onechar\">One character at a time (centered)</option>"
        "<option value=\"static\">Static (left edge, may clip)</option>"
        "<option value=\"fill\">Full panel — solid color (lamp / night light)</option>"
        "</select></div>"
        "<div class=\"row\"><label for=\"dir\">Marquee scroll direction</label>"
        "<select id=\"dir\">"
        "<option value=\"left\">Left — text moves left</option>"
        "<option value=\"right\">Right — text moves right</option>"
        "<option value=\"down\">Down — top to bottom</option>"
        "<option value=\"up\">Up — bottom to top</option>"
        "</select>"
        "<p class=\"hint\">Used when mode is scrolling marquee.</p></div>"
        "<div class=\"row\"><label for=\"zoom\">One-character zoom</label>"
        "<select id=\"zoom\">"
        "<option value=\"none\">None — snap to full size</option>"
        "<option value=\"in\">Zoom in (small → large)</option>"
        "<option value=\"out\">Zoom out (large → small)</option>"
        "</select>"
        "<p class=\"hint\">Used when mode is one character at a time; eight frames per glyph.</p></div>"
        "<div class=\"row\"><label for=\"color\">Text / lamp color</label>"
        "<select id=\"color\">"
        "<option value=\"red\">Red</option>"
        "<option value=\"green\">Green</option>"
        "<option value=\"blue\">Blue</option>"
        "<option value=\"white\">White</option>"
        "<option value=\"yellow\">Yellow</option>"
        "<option value=\"cyan\">Cyan</option>"
        "<option value=\"magenta\">Magenta</option>"
        "<option value=\"orange\">Orange</option>"
        "<option value=\"purple\">Purple</option>"
        "<option value=\"pink\">Pink</option>"
        "<option value=\"warm\">Warm white</option>"
        "<option value=\"cool\">Cool white</option>"
        "<option value=\"rainbow\">Rainbow (animated)</option>"
        "</select></div>"
        "<div class=\"row row-inline\">"
        "<label><input type=\"checkbox\" id=\"rainbow\" /> Rainbow on lit pixels</label></div>"
        "<div class=\"actions\">"
        "<button type=\"button\" class=\"btn\" onclick=\"applyConfig()\">Apply settings</button>"
        "<button type=\"button\" class=\"btn secondary\" onclick=\"clearPanel()\">Turn off LEDs</button>"
        "</div></div></div>"
        "<script>"
        "function q(o){var p=[];for(var k in o){p.push(encodeURIComponent(k)+'='+encodeURIComponent(o[k]));}"
        "return p.join('&');}"
        "function sendText(){var t=document.getElementById('text').value;"
        "fetch('/SendData?data='+encodeURIComponent(t));}"
        "function applyConfig(){"
        "fetch('/api/config?'+q({speed:document.getElementById('speed').value,"
        "mode:document.getElementById('mode').value,color:document.getElementById('color').value,"
        "rainbow:document.getElementById('rainbow').checked?'1':'0',"
        "direction:document.getElementById('dir').value,"
        "zoom:document.getElementById('zoom').value}));}"
        "function clearPanel(){fetch('/api/config?'+q({mode:'fill',color:'off',rainbow:'0'}));}"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_data_handler(httpd_req_t *req)
{
    char buf[TEXT_MAX];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(buf, s_text, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    xSemaphoreGive(s_mtx);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/** Decode %HH (UTF-8) and '+' → space; safe in-place (shortens string). */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * Find key=value in query string (handles URL-encoded UTF-8 values).
 * Returns ESP_OK if key found (value may be empty).
 */
static esp_err_t query_get_value(const char *qs, const char *key, char *out, size_t out_sz)
{
    const size_t key_len = strlen(key);
    for (const char *p = qs; *p != '\0';) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *amp = strchr(p, '&');
            size_t n = amp != NULL ? (size_t)(amp - p) : strlen(p);
            if (n >= out_sz) {
                n = out_sz - 1;
            }
            memcpy(out, p, n);
            out[n] = '\0';
            return ESP_OK;
        }
        const char *next = strchr(p, '&');
        if (next == NULL) {
            break;
        }
        p = next + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

static void url_decode_inplace(char *s)
{
    const char *r = s;
    char *w = s;
    while (*r != '\0') {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] != '\0' && r[2] != '\0') {
            const int hi = hex_nibble(r[1]);
            const int lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((unsigned)((hi << 4) | lo) & 0xFF);
                r += 3;
                continue;
            }
            *w++ = *r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static esp_err_t send_data_handler(httpd_req_t *req)
{
    char qs[1024];
    char val[TEXT_MAX];

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "NO_QUERY", HTTPD_RESP_USE_STRLEN);
    }

    if (query_get_value(qs, "data", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "NO_DATA", HTTPD_RESP_USE_STRLEN);
    }

    url_decode_inplace(val);
    /* Strip UTF-8 BOM if browser sends it */
    if ((unsigned char)val[0] == 0xEF && (unsigned char)val[1] == 0xBB && (unsigned char)val[2] == 0xBF) {
        memmove(val, val + 3, strlen(val + 3) + 1);
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(s_text, val, sizeof(s_text));
    s_text[sizeof(s_text) - 1] = '\0';
    /* Show new text: exit full-panel (lamp) mode and restart animation. */
    if (s_disp_mode == DISP_FILL) {
        s_disp_mode = DISP_MARQUEE;
    }
    marquee_reset_marquee_locked();
    s_cc_off = 0;
    s_oc_phase = 0;
    s_flow_flag = 1;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "Scroll on, len=%u text=%s", (unsigned)strlen(val), val);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static void apply_color_preset(const char *name, uint8_t *r, uint8_t *g, uint8_t *b, bool *rainbow_mode)
{
    *rainbow_mode = false;
    if (name == NULL || name[0] == '\0') {
        return;
    }
    if (!strcmp(name, "red")) {
        *r = 255;
        *g = 0;
        *b = 0;
    } else if (!strcmp(name, "green")) {
        *r = 0;
        *g = 255;
        *b = 0;
    } else if (!strcmp(name, "blue")) {
        *r = 0;
        *g = 0;
        *b = 255;
    } else if (!strcmp(name, "white")) {
        *r = 255;
        *g = 255;
        *b = 255;
    } else if (!strcmp(name, "yellow")) {
        *r = 255;
        *g = 255;
        *b = 0;
    } else if (!strcmp(name, "cyan")) {
        *r = 0;
        *g = 255;
        *b = 255;
    } else if (!strcmp(name, "magenta")) {
        *r = 255;
        *g = 0;
        *b = 255;
    } else if (!strcmp(name, "orange")) {
        *r = 255;
        *g = 140;
        *b = 0;
    } else if (!strcmp(name, "purple")) {
        *r = 160;
        *g = 0;
        *b = 255;
    } else if (!strcmp(name, "pink")) {
        *r = 255;
        *g = 105;
        *b = 180;
    } else if (!strcmp(name, "warm")) {
        *r = 255;
        *g = 200;
        *b = 120;
    } else if (!strcmp(name, "cool")) {
        *r = 200;
        *g = 230;
        *b = 255;
    } else if (!strcmp(name, "rainbow")) {
        *rainbow_mode = true;
        *r = 255;
        *g = 0;
        *b = 0;
    } else if (!strcmp(name, "off")) {
        *r = 0;
        *g = 0;
        *b = 0;
    }
}

static esp_err_t api_config_handler(httpd_req_t *req)
{
    char qs[1024];
    char val[128];

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "NO_QUERY", HTTPD_RESP_USE_STRLEN);
    }

    bool ch_mode = false;
    bool ch_color = false;
    bool wipe_fill = false;
    uint8_t wr = 0, wg = 0, wb = 0;
    int rb_in = -1;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (query_get_value(qs, "speed", val, sizeof(val)) == ESP_OK) {
        int v = atoi(val);
        if (v < 1) {
            v = 1;
        }
        if (v > 10) {
            v = 10;
        }
        s_speed = v;
        s_period_ticks = 24 - 2 * v;
        if (s_period_ticks < 2) {
            s_period_ticks = 2;
        }
    }

    if (query_get_value(qs, "rainbow", val, sizeof(val)) == ESP_OK) {
        rb_in = (val[0] == '1' || val[0] == 't' || val[0] == 'T' || val[0] == 'y' || val[0] == 'Y') ? 1
                                                                                                 : 0;
    }

    if (query_get_value(qs, "color", val, sizeof(val)) == ESP_OK) {
        bool rm = false;
        uint8_t r = s_cr;
        uint8_t g = s_cg;
        uint8_t b = s_cb;
        apply_color_preset(val, &r, &g, &b, &rm);
        s_cr = r;
        s_cg = g;
        s_cb = b;
        if (rm) {
            s_rainbow = true;
        } else if (rb_in >= 0) {
            s_rainbow = (rb_in != 0);
        }
        ch_color = true;
    } else if (rb_in >= 0) {
        s_rainbow = (rb_in != 0);
    }

    if (query_get_value(qs, "mode", val, sizeof(val)) == ESP_OK) {
        ch_mode = true;
        if (!strcmp(val, "marquee")) {
            s_disp_mode = DISP_MARQUEE;
        } else if (!strcmp(val, "onechar")) {
            s_disp_mode = DISP_ONE_CHAR;
        } else if (!strcmp(val, "static")) {
            s_disp_mode = DISP_STATIC;
        } else if (!strcmp(val, "fill")) {
            s_disp_mode = DISP_FILL;
        } else {
            s_disp_mode = DISP_MARQUEE;
        }

        if (s_disp_mode == DISP_FILL) {
            s_flow_flag = 0;
        } else {
            s_flow_flag = 1;
            marquee_reset_marquee_locked();
            s_cc_off = 0;
            s_oc_phase = 0;
        }
    }

    if (query_get_value(qs, "direction", val, sizeof(val)) == ESP_OK) {
        if (!strcmp(val, "right")) {
            s_marquee_dir = MARQUEE_DIR_RIGHT;
        } else if (!strcmp(val, "up")) {
            s_marquee_dir = MARQUEE_DIR_UP;
        } else if (!strcmp(val, "down")) {
            s_marquee_dir = MARQUEE_DIR_DOWN;
        } else {
            s_marquee_dir = MARQUEE_DIR_LEFT;
        }
        if (s_disp_mode == DISP_MARQUEE) {
            marquee_reset_marquee_locked();
        }
    }

    if (query_get_value(qs, "zoom", val, sizeof(val)) == ESP_OK) {
        if (!strcmp(val, "in")) {
            s_oc_zoom = OC_ZOOM_IN;
        } else if (!strcmp(val, "out")) {
            s_oc_zoom = OC_ZOOM_OUT;
        } else {
            s_oc_zoom = OC_ZOOM_NONE;
        }
        s_oc_phase = 0;
    }

    if (s_disp_mode == DISP_FILL && (ch_mode || ch_color)) {
        wipe_fill = true;
        wr = s_cr;
        wg = s_cg;
        wb = s_cb;
    }

    xSemaphoreGive(s_mtx);

    if (wipe_fill) {
        color_wipe_rgb(wr, wg, wb);
    }

    ESP_LOGI(TAG, "api/config mode=%d speed=%d rainbow=%d", (int)s_disp_mode, s_speed, (int)s_rainbow);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t rgb_on_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_disp_mode = DISP_FILL;
    s_cr = 0;
    s_cg = 255;
    s_cb = 0;
    s_rainbow = false;
    s_flow_flag = 0;
    xSemaphoreGive(s_mtx);
    color_wipe_rgb(0, 255, 0);
    ESP_LOGI(TAG, "Legacy RGB On → full panel green (lamp)");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t rgb_off_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_disp_mode = DISP_MARQUEE;
    s_flow_flag = 1;
    marquee_reset_marquee_locked();
    s_cc_off = 0;
    s_oc_phase = 0;
    xSemaphoreGive(s_mtx);
    color_wipe_rgb(0, 0, 0);
    ESP_LOGI(TAG, "Legacy RGB Off → marquee, LEDs cleared");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        return NULL;
    }

    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
    httpd_uri_t get_uri = {.uri = "/getData", .method = HTTP_GET, .handler = get_data_handler, .user_ctx = NULL};
    httpd_uri_t send_uri = {.uri = "/SendData", .method = HTTP_GET, .handler = send_data_handler, .user_ctx = NULL};
    httpd_uri_t api_uri = {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_handler, .user_ctx = NULL};
    httpd_uri_t on_uri = {.uri = "/RGBOn", .method = HTTP_GET, .handler = rgb_on_handler, .user_ctx = NULL};
    httpd_uri_t off_uri = {.uri = "/RGBOff", .method = HTTP_GET, .handler = rgb_off_handler, .user_ctx = NULL};

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &send_uri);
    httpd_register_uri_handler(server, &api_uri);
    httpd_register_uri_handler(server, &on_uri);
    httpd_register_uri_handler(server, &off_uri);

    return server;
}

static void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    wifi_config_t wifi_config = {
        .ap =
            {
                .channel = 1,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
    };
    strncpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
    wifi_config.ap.ssid_len = (uint8_t)strlen((const char *)wifi_config.ap.ssid);
    strncpy((char *)wifi_config.ap.password, AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';
    if (strlen(AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, AP_IP_O1, AP_IP_O2, AP_IP_O3, AP_IP_O4);
    IP4_ADDR(&ip_info.gw, AP_IP_O1, AP_IP_O2, AP_IP_O3, AP_IP_O4);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    ESP_LOGI(TAG, "Soft AP SSID:%s password:%s IP:%d.%d.%d.%d", AP_SSID, AP_PASSWORD,
             AP_IP_O1, AP_IP_O2, AP_IP_O3, AP_IP_O4);
}

void app_main(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_flow_flag = 1;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    marquee_reset_marquee_locked();
    xSemaphoreGive(s_mtx);
    s_period_ticks = 24 - 2 * s_speed;
    if (s_period_ticks < 2) {
        s_period_ticks = 2;
    }

    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_softap();

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

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    color_wipe_rgb(0, 0, 0);

    if (start_webserver() == NULL) {
        ESP_LOGE(TAG, "HTTP server start failed");
    } else {
        ESP_LOGI(TAG, "Web server on http://%d.%d.%d.%d/", AP_IP_O1, AP_IP_O2, AP_IP_O3, AP_IP_O4);
    }

    uint32_t flag = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_DELAY_MS));
        if (s_flow_flag) {
            flag++;
            int period = s_period_ticks;
            if (period < 2) {
                period = 2;
            }
            if (flag >= period) {
                display_step();
                flag = 0;
            }
        }
    }
}
