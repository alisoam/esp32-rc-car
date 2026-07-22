#include "camera_frame.h"
#include "ov7670.h"
#include "ov7670_frame.h"
#include "jpeg_encoder.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "camera_frame";

#define SYNTH_WIDTH   176
#define SYNTH_HEIGHT  144
#define BPP           3

#define RGB_SIZE  (SYNTH_WIDTH * SYNTH_HEIGHT * BPP)
#define JPEG_MAX  (24 * 1024)

static bool              use_real_camera;
static uint8_t           rgb_buf[RGB_SIZE] __attribute__((aligned(16)));
static uint8_t           synth_jpg_buf[JPEG_MAX];
static jpeg_encoder_t   *synth_jpeg_enc;

void camera_frame_init(void)
{
    synth_jpeg_enc = jpeg_encoder_create(SYNTH_WIDTH, SYNTH_HEIGHT, 70);
    assert(synth_jpeg_enc != NULL);
    ESP_LOGI(TAG, "synthetic camera frame init ok (ESP_NEW_JPEG)");
}

void camera_frame_init_real(void)
{
    if (ov7670_init() != ESP_OK) {
        ESP_LOGW(TAG, "OV7670 not found, using synthetic frames");
        use_real_camera = false;
        return;
    }
    if (ov7670_frame_init() != ESP_OK) {
        ESP_LOGW(TAG, "OV7670 frame capture failed, using synthetic frames");
        use_real_camera = false;
        return;
    }
    use_real_camera = true;
    ESP_LOGI(TAG, "real camera capture active");
}

void camera_frame_deinit(void)
{
    if (synth_jpeg_enc) {
        jpeg_encoder_destroy(synth_jpeg_enc);
        synth_jpeg_enc = NULL;
    }
    ESP_LOGI(TAG, "camera frame deinit");
}

static inline void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= SYNTH_WIDTH || y < 0 || y >= SYNTH_HEIGHT) return;
    int off = (y * SYNTH_WIDTH + x) * BPP;
    rgb_buf[off + 0] = r;
    rgb_buf[off + 1] = g;
    rgb_buf[off + 2] = b;
}

static void fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            set_pixel(x + dx, y + dy, r, g, b);
        }
    }
}

static void draw_char(int x, int y, char ch, uint8_t r, uint8_t g, uint8_t b, int scale)
{
    static const uint8_t font[96][5] = {
        [' ' - 32] = {0x00, 0x00, 0x00, 0x00, 0x00},
        ['!' - 32] = {0x00, 0x00, 0x5F, 0x00, 0x00},
        ['"' - 32] = {0x00, 0x07, 0x00, 0x07, 0x00},
        ['#' - 32] = {0x14, 0x7F, 0x14, 0x7F, 0x14},
        ['$' - 32] = {0x24, 0x2A, 0x7F, 0x2A, 0x12},
        ['%' - 32] = {0x23, 0x13, 0x08, 0x64, 0x62},
        ['&' - 32] = {0x36, 0x49, 0x55, 0x22, 0x50},
        ['\'' -32] = {0x00, 0x05, 0x03, 0x00, 0x00},
        ['(' - 32] = {0x00, 0x1C, 0x22, 0x41, 0x00},
        [')' - 32] = {0x00, 0x41, 0x22, 0x1C, 0x00},
        ['*' - 32] = {0x14, 0x08, 0x3E, 0x08, 0x14},
        ['+' - 32] = {0x08, 0x08, 0x3E, 0x08, 0x08},
        [',' - 32] = {0x00, 0x50, 0x30, 0x00, 0x00},
        ['-' - 32] = {0x08, 0x08, 0x08, 0x08, 0x08},
        ['.' - 32] = {0x00, 0x60, 0x60, 0x00, 0x00},
        ['/' - 32] = {0x20, 0x10, 0x08, 0x04, 0x02},
        ['0' - 32] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
        ['1' - 32] = {0x00, 0x42, 0x7F, 0x40, 0x00},
        ['2' - 32] = {0x42, 0x61, 0x51, 0x49, 0x46},
        ['3' - 32] = {0x21, 0x41, 0x45, 0x4B, 0x31},
        ['4' - 32] = {0x18, 0x14, 0x12, 0x7F, 0x10},
        ['5' - 32] = {0x27, 0x45, 0x45, 0x45, 0x39},
        ['6' - 32] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
        ['7' - 32] = {0x01, 0x71, 0x09, 0x05, 0x03},
        ['8' - 32] = {0x36, 0x49, 0x49, 0x49, 0x36},
        ['9' - 32] = {0x06, 0x49, 0x49, 0x29, 0x1E},
        [':' - 32] = {0x00, 0x36, 0x36, 0x00, 0x00},
        [';' - 32] = {0x00, 0x56, 0x36, 0x00, 0x00},
        ['<' - 32] = {0x08, 0x14, 0x22, 0x41, 0x00},
        ['=' - 32] = {0x14, 0x14, 0x14, 0x14, 0x14},
        ['>' - 32] = {0x00, 0x41, 0x22, 0x14, 0x08},
        ['?' - 32] = {0x02, 0x01, 0x51, 0x09, 0x06},
        ['@' - 32] = {0x32, 0x49, 0x79, 0x41, 0x3E},
        ['A' - 32] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
        ['B' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x36},
        ['C' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x22},
        ['D' - 32] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
        ['E' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x41},
        ['F' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x01},
        ['G' - 32] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
        ['H' - 32] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
        ['I' - 32] = {0x00, 0x41, 0x7F, 0x41, 0x00},
        ['J' - 32] = {0x20, 0x40, 0x41, 0x3F, 0x01},
        ['K' - 32] = {0x7F, 0x08, 0x14, 0x22, 0x41},
        ['L' - 32] = {0x7F, 0x40, 0x40, 0x40, 0x40},
        ['M' - 32] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        ['N' - 32] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
        ['O' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
        ['P' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x06},
        ['Q' - 32] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
        ['R' - 32] = {0x7F, 0x09, 0x19, 0x29, 0x46},
        ['S' - 32] = {0x46, 0x49, 0x49, 0x49, 0x31},
        ['T' - 32] = {0x01, 0x01, 0x7F, 0x01, 0x01},
        ['U' - 32] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
        ['V' - 32] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
        ['W' - 32] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
        ['X' - 32] = {0x63, 0x14, 0x08, 0x14, 0x63},
        ['Y' - 32] = {0x07, 0x08, 0x70, 0x08, 0x07},
        ['Z' - 32] = {0x61, 0x51, 0x49, 0x45, 0x43},
        ['[' - 32] = {0x00, 0x7F, 0x41, 0x41, 0x00},
        ['\\' -32] = {0x02, 0x04, 0x08, 0x10, 0x20},
        [']' - 32] = {0x00, 0x41, 0x41, 0x7F, 0x00},
        ['^' - 32] = {0x04, 0x02, 0x01, 0x02, 0x04},
        ['_' - 32] = {0x40, 0x40, 0x40, 0x40, 0x40},
        ['`' - 32] = {0x00, 0x01, 0x02, 0x04, 0x00},
        ['a' - 32] = {0x20, 0x54, 0x54, 0x54, 0x78},
        ['b' - 32] = {0x7F, 0x48, 0x44, 0x44, 0x38},
        ['c' - 32] = {0x38, 0x44, 0x44, 0x44, 0x20},
        ['d' - 32] = {0x38, 0x44, 0x44, 0x48, 0x7F},
        ['e' - 32] = {0x38, 0x54, 0x54, 0x54, 0x18},
        ['f' - 32] = {0x08, 0x7E, 0x09, 0x01, 0x02},
        ['g' - 32] = {0x0C, 0x52, 0x52, 0x52, 0x3E},
        ['h' - 32] = {0x7F, 0x08, 0x04, 0x04, 0x78},
        ['i' - 32] = {0x00, 0x44, 0x7D, 0x40, 0x00},
        ['j' - 32] = {0x20, 0x40, 0x44, 0x3D, 0x00},
        ['k' - 32] = {0x7F, 0x10, 0x28, 0x44, 0x00},
        ['l' - 32] = {0x00, 0x41, 0x7F, 0x40, 0x00},
        ['m' - 32] = {0x7C, 0x04, 0x18, 0x04, 0x78},
        ['n' - 32] = {0x7C, 0x08, 0x04, 0x04, 0x78},
        ['o' - 32] = {0x38, 0x44, 0x44, 0x44, 0x38},
        ['p' - 32] = {0x7C, 0x14, 0x14, 0x14, 0x08},
        ['q' - 32] = {0x08, 0x14, 0x14, 0x18, 0x7C},
        ['r' - 32] = {0x7C, 0x08, 0x04, 0x04, 0x08},
        ['s' - 32] = {0x48, 0x54, 0x54, 0x54, 0x20},
        ['t' - 32] = {0x04, 0x3F, 0x44, 0x40, 0x20},
        ['u' - 32] = {0x3C, 0x40, 0x40, 0x20, 0x7C},
        ['v' - 32] = {0x1C, 0x20, 0x40, 0x20, 0x1C},
        ['w' - 32] = {0x3C, 0x40, 0x30, 0x40, 0x3C},
        ['x' - 32] = {0x44, 0x28, 0x10, 0x28, 0x44},
        ['y' - 32] = {0x0C, 0x50, 0x50, 0x50, 0x3C},
        ['z' - 32] = {0x44, 0x64, 0x54, 0x4C, 0x44},
        ['{' - 32] = {0x00, 0x08, 0x36, 0x41, 0x00},
        ['|' - 32] = {0x00, 0x00, 0x7F, 0x00, 0x00},
        ['}' - 32] = {0x00, 0x41, 0x36, 0x08, 0x00},
        ['~' - 32] = {0x08, 0x04, 0x08, 0x10, 0x08},
    };

    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx >= 96) return;
    const uint8_t *glyph = font[idx];

    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = x + col * scale;
                int py = y + row * scale;
                fill_rect(px, py, scale, scale, r, g, b);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b, int scale)
{
    while (*text) {
        draw_char(x, y, *text, r, g, b, scale);
        x += 6 * scale;
        text++;
    }
}

static camera_frame_t camera_frame_generate_synthetic(int32_t left_motor, int32_t right_motor, int frame_count)
{
    int64_t now = esp_timer_get_time();
    double elapsed = now / 1000000.0;

    uint8_t bg_r = 20 + (uint8_t)(10 * sin(elapsed * 0.3));
    uint8_t bg_g = 24 + (uint8_t)(10 * sin(elapsed * 0.5 + 1));
    uint8_t bg_b = 30 + (uint8_t)(10 * sin(elapsed * 0.4 + 2));
    fill_rect(0, 0, SYNTH_WIDTH, SYNTH_HEIGHT, bg_r, bg_g, bg_b);

    draw_text(8, 8, "CAMERA LIVE", 79, 195, 247, 2);

    char buf[32];
    snprintf(buf, sizeof(buf), "L %+4ld", (long)left_motor);
    draw_text(8, 28, buf, left_motor > 0 ? 0 : (left_motor < 0 ? 255 : 80),
              left_motor > 0 ? 200 : 0, left_motor != 0 ? 255 : 80, 2);

    snprintf(buf, sizeof(buf), "R %+4ld", (long)right_motor);
    draw_text(8, 44, buf, right_motor > 0 ? 0 : (right_motor < 0 ? 255 : 80),
              right_motor > 0 ? 200 : 0, right_motor != 0 ? 255 : 80, 2);

    snprintf(buf, sizeof(buf), "FR %d", frame_count);
    draw_text(8, 62, buf, 180, 180, 180, 1);

    int bx = (int)(10 + (SYNTH_WIDTH - 30) * (0.5 + 0.5 * sin(elapsed * 2.0)));
    int by = (int)(SYNTH_HEIGHT - 30 + 10 * cos(elapsed * 1.5));
    fill_rect(bx, by, 14, 14, 79, 195, 247);

    int bar_x = 8;
    int bar_y = 104;
    int bar_w = SYNTH_WIDTH - 16;
    int bar_h = 10;

    fill_rect(bar_x, bar_y, bar_w, bar_h, 40, 40, 40);
    int l_len = (int)(labs((long)left_motor) * bar_w / 255);
    if (left_motor > 0) {
        fill_rect(bar_x, bar_y, l_len, bar_h, 0, 200, 0);
    } else if (left_motor < 0) {
        fill_rect(bar_x, bar_y, l_len, bar_h, 200, 0, 0);
    }

    bar_y += 16;
    fill_rect(bar_x, bar_y, bar_w, bar_h, 40, 40, 40);
    int r_len = (int)(labs((long)right_motor) * bar_w / 255);
    if (right_motor > 0) {
        fill_rect(bar_x, bar_y, r_len, bar_h, 0, 200, 0);
    } else if (right_motor < 0) {
        fill_rect(bar_x, bar_y, r_len, bar_h, 200, 0, 0);
    }

    size_t out_size = jpeg_encoder_encode_rgb888(synth_jpeg_enc, rgb_buf, synth_jpg_buf, JPEG_MAX);

    camera_frame_t frame;
    frame.data = synth_jpg_buf;
    frame.size = out_size;
    return frame;
}

camera_frame_t camera_frame_generate(int32_t left_motor, int32_t right_motor, int frame_count)
{
    if (use_real_camera) {
        ov7670_frame_t real = ov7670_frame_get();
        if (real.data && real.size > 0) {
            camera_frame_t frame;
            frame.data = real.data;
            frame.size = real.size;
            return frame;
        }
    }
    return camera_frame_generate_synthetic(left_motor, right_motor, frame_count);
}
