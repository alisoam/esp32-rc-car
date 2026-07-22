#include "jpeg_encoder.h"

#include <stdlib.h>
#include <string.h>
#include "esp_jpeg_enc.h"
#include "esp_jpeg_common.h"
#include "esp_log.h"

static const char *TAG = "jpeg_enc";

struct jpeg_encoder_ctx {
    jpeg_enc_handle_t handle;
    jpeg_enc_handle_t handle_rgb565;
    int width;
    int height;
    int quality;
    uint8_t *fallback_buf;
    size_t   fallback_buf_size;
};

jpeg_encoder_t *jpeg_encoder_create(int width, int height, int quality)
{
    jpeg_encoder_t *enc = calloc(1, sizeof(jpeg_encoder_t));
    if (!enc) {
        ESP_LOGE(TAG, "failed to alloc encoder ctx");
        return NULL;
    }

    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width       = width;
    cfg.height      = height;
    cfg.src_type    = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality     = quality;
    cfg.rotate      = JPEG_ROTATE_0D;
    cfg.task_enable = false;

    jpeg_error_t ret = jpeg_enc_open(&cfg, &enc->handle);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_enc_open failed: %d", ret);
        free(enc);
        return NULL;
    }

    enc->width       = width;
    enc->height      = height;
    enc->quality     = quality;

    jpeg_enc_config_t cfg565 = DEFAULT_JPEG_ENC_CONFIG();
    cfg565.width       = width;
    cfg565.height      = height;
    cfg565.src_type    = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg565.subsampling = JPEG_SUBSAMPLE_420;
    cfg565.quality     = quality;
    cfg565.rotate      = JPEG_ROTATE_0D;
    cfg565.task_enable = false;

    ret = jpeg_enc_open(&cfg565, &enc->handle_rgb565);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_enc_open RGB565 failed: %d", ret);
        jpeg_enc_close(enc->handle);
        free(enc);
        return NULL;
    }

    int in_size = width * height * 3;
    enc->fallback_buf = jpeg_calloc_align(in_size, 16);
    enc->fallback_buf_size = in_size;
    if (!enc->fallback_buf) {
        ESP_LOGE(TAG, "failed to alloc fallback buffer");
        jpeg_enc_close(enc->handle);
        jpeg_enc_close(enc->handle_rgb565);
        free(enc);
        return NULL;
    }

    ESP_LOGI(TAG, "ESP_NEW_JPEG encoder ready: %dx%d quality=%d",
             width, height, quality);
    return enc;
}

static inline bool is_aligned(const uint8_t *ptr, int alignment)
{
    return ((uintptr_t)ptr % alignment) == 0;
}

size_t jpeg_encoder_encode_rgb888(jpeg_encoder_t *enc, const uint8_t *rgb,
                                   uint8_t *out, size_t out_max)
{
    if (!enc || !rgb || !out) return 0;

    const uint8_t *src = rgb;
    int in_size = enc->width * enc->height * 3;

    if (!is_aligned(rgb, 16)) {
        memcpy(enc->fallback_buf, rgb, in_size);
        src = enc->fallback_buf;
    }

    int out_len = 0;
    jpeg_error_t ret = jpeg_enc_process(enc->handle, src, in_size,
                                         out, (int)out_max, &out_len);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "encode failed: %d", ret);
        return 0;
    }

    return (size_t)out_len;
}

size_t jpeg_encoder_encode_rgb565(jpeg_encoder_t *enc, const uint16_t *rgb565,
                                   uint8_t *out, size_t out_max)
{
    if (!enc || !rgb565 || !out) return 0;
    if (!enc->handle_rgb565) return 0;

    const uint8_t *src = (const uint8_t *)rgb565;
    int in_size = enc->width * enc->height * 2;
    int out_len = 0;

    if (!is_aligned(src, 16)) {
        int copy_size = in_size < (int)enc->fallback_buf_size ? in_size : (int)enc->fallback_buf_size;
        memcpy(enc->fallback_buf, src, copy_size);
        src = enc->fallback_buf;
    }

    jpeg_enc_process(enc->handle_rgb565, src, in_size, out, (int)out_max, &out_len);

    return (size_t)out_len;
}

void jpeg_encoder_destroy(jpeg_encoder_t *enc)
{
    if (!enc) return;
    if (enc->handle) jpeg_enc_close(enc->handle);
    if (enc->handle_rgb565) jpeg_enc_close(enc->handle_rgb565);
    jpeg_free_align(enc->fallback_buf);
    free(enc);
    ESP_LOGI(TAG, "encoder destroyed");
}

uint8_t *jpeg_encoder_alloc_buf(size_t size, int aligned)
{
    return (uint8_t *)jpeg_calloc_align(size, aligned);
}

void jpeg_encoder_free_buf(uint8_t *buf)
{
    jpeg_free_align(buf);
}

size_t jpeg_encode_rgb888(const uint8_t *rgb, int width, int height,
                          uint8_t *out, size_t out_max, int quality)
{
    jpeg_encoder_t *enc = jpeg_encoder_create(width, height, quality);
    if (!enc) return 0;
    size_t sz = jpeg_encoder_encode_rgb888(enc, rgb, out, out_max);
    jpeg_encoder_destroy(enc);
    return sz;
}
