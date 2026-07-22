#include "ov7670_frame.h"
#include "ov7670.h"
#include "jpeg_encoder.h"

#include <string.h>
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "ov7670_frame"

#define JPEG_MAX    (24 * 1024)
#define RAW_SIZE    (OV7670_WIDTH * OV7670_HEIGHT * 2)

static esp_cam_ctlr_handle_t cam_handle;
static jpeg_encoder_t      *jpeg_enc;

static uint8_t *cam_buffer;
static uint8_t  jpg_buf[JPEG_MAX];
static size_t   jpg_len;

static SemaphoreHandle_t frame_ready_sem;
static SemaphoreHandle_t mutex;

static bool cam_running;
static volatile int vsync_count = 0;
static volatile int trans_done_count = 0;

static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t handle,
                                        esp_cam_ctlr_trans_t *trans,
                                        void *user_data)
{
    (void)handle;
    (void)user_data;
    vsync_count++;
    trans->buffer = cam_buffer;
    trans->buflen = RAW_SIZE;
    return false;
}

static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t handle,
                                         esp_cam_ctlr_trans_t *trans,
                                         void *user_data)
{
    (void)handle;
    (void)user_data;
    (void)trans;
    trans_done_count++;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(frame_ready_sem, &woken);
    return woken == pdTRUE;
}

static void camera_task(void *arg)
{
    (void)arg;
    int frame_count = 0;
    int timeout_count = 0;

    while (cam_running) {
        if (xSemaphoreTake(frame_ready_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            timeout_count++;
            ESP_LOGW(TAG, "no frame in 3s (to=%d), vsync=%d done=%d frames=%d",
                     timeout_count, vsync_count, trans_done_count, frame_count);

            if (timeout_count >= 2) {
                ESP_LOGW(TAG, "restarting camera...");
                esp_cam_ctlr_stop(cam_handle);
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_cam_ctlr_start(cam_handle);
                vsync_count = 0;
                trans_done_count = 0;
                timeout_count = 0;
            }
            continue;
        }

        timeout_count = 0;
        frame_count++;
        int64_t t0 = esp_timer_get_time();

        xSemaphoreTake(mutex, portMAX_DELAY);
        size_t sz = jpeg_encoder_encode_rgb565(jpeg_enc,
                        (const uint16_t *)cam_buffer, jpg_buf, JPEG_MAX);
        jpg_len = (sz > 0) ? sz : 0;
        xSemaphoreGive(mutex);
        int64_t t1 = esp_timer_get_time();

        if (frame_count <= 5 || (frame_count % 60) == 0) {
            ESP_LOGI(TAG, "frame %d: %d bytes in %lld us",
                     frame_count, (int)jpg_len, t1 - t0);
        }
    }

    vTaskDelete(NULL);
}

esp_err_t ov7670_frame_init(void)
{
    jpeg_enc = jpeg_encoder_create(OV7670_WIDTH, OV7670_HEIGHT, 40);
    if (!jpeg_enc) {
        ESP_LOGE(TAG, "jpeg encoder create failed");
        return ESP_FAIL;
    }

    esp_cam_ctlr_dvp_pin_config_t pin = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            OV7670_D0, OV7670_D1, OV7670_D2, OV7670_D3,
            OV7670_D4, OV7670_D5, OV7670_D6, OV7670_D7,
        },
        .vsync_io = OV7670_VSYNC,
        .de_io    = OV7670_HREF,
        .pclk_io  = OV7670_PCLK,
        .xclk_io  = OV7670_XCLK,
    };

    esp_cam_ctlr_dvp_config_t dvp_cfg = {
        .ctlr_id                 = 0,
        .clk_src                 = CAM_CLK_SRC_DEFAULT,
        .h_res                   = OV7670_WIDTH,
        .v_res                   = OV7670_HEIGHT,
        .input_data_color_type   = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type  = CAM_CTLR_COLOR_RGB565,
        .cam_data_width          = 8,
        .byte_swap_en            = false,
        .bit_swap_en             = false,
        .bk_buffer_dis           = true,
        .pin_dont_init           = false,
        .external_xtal           = false,
        .xclk_freq               = 10000000,
        .dma_burst_size          = 64,
        .pin                     = &pin,
    };

    esp_err_t ret = esp_cam_new_dvp_ctlr(&dvp_cfg, &cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_cam_new_dvp_ctlr failed: %d", ret);
        return ret;
    }

    cam_buffer = esp_cam_ctlr_alloc_buffer(cam_handle, RAW_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!cam_buffer) {
        ESP_LOGE(TAG, "cam buffer alloc failed");
        esp_cam_ctlr_del(cam_handle);
        cam_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    size_t fb_len = 0;
    esp_cam_ctlr_get_frame_buffer_len(cam_handle, &fb_len);
    ESP_LOGI(TAG, "cam_buffer=%p raw_size=%d fb_size=%d", cam_buffer, RAW_SIZE, (int)fb_len);

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, NULL));

    frame_ready_sem = xSemaphoreCreateBinary();
    mutex = xSemaphoreCreateMutex();
    assert(frame_ready_sem && mutex);

    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam_handle));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam_handle));

    cam_running = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        camera_task, "camera", 4096, NULL, 5, NULL, 0);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create camera task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "camera capture: %dx%d RGB565 → JPEG",
             OV7670_WIDTH, OV7670_HEIGHT);
    return ESP_OK;
}

void ov7670_frame_deinit(void)
{
    cam_running = false;
    if (cam_handle) {
        esp_cam_ctlr_stop(cam_handle);
        esp_cam_ctlr_disable(cam_handle);
        esp_cam_ctlr_del(cam_handle);
        cam_handle = NULL;
    }
    if (mutex) {
        vSemaphoreDelete(mutex);
        mutex = NULL;
    }
    if (frame_ready_sem) {
        vSemaphoreDelete(frame_ready_sem);
        frame_ready_sem = NULL;
    }
    if (jpeg_enc) {
        jpeg_encoder_destroy(jpeg_enc);
        jpeg_enc = NULL;
    }
    if (cam_buffer) {
        heap_caps_free(cam_buffer);
        cam_buffer = NULL;
    }
    ESP_LOGI(TAG, "camera capture stopped");
}

ov7670_frame_t ov7670_frame_get(void)
{
    ov7670_frame_t frame = { .data = NULL, .size = 0 };
    if (!mutex) return frame;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (jpg_len > 0) {
        frame.data = jpg_buf;
        frame.size = jpg_len;
    }
    xSemaphoreGive(mutex);
    return frame;
}
