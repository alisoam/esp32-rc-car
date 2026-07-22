#include "http_server.h"
#include "motor_control.h"
#include "camera_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <inttypes.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <errno.h>

volatile int32_t  motor_left_speed  = 0;
volatile int32_t  motor_right_speed = 0;
volatile uint64_t last_control_seq  = 0;

static const char *TAG = "http_server";

static struct {
    httpd_handle_t hd;
    int            fd;
    bool           active;
    int            generation;
    SemaphoreHandle_t lock;
} ws_state = { .fd = -1, .active = false, .generation = 0 };

static void set_motor(int32_t l, int32_t r, uint64_t s)
{
    if (s != 0 && s <= last_control_seq) return;
    if (s != 0) last_control_seq = s;
    motor_left_speed  = l;
    motor_right_speed = r;
    motor_set(l, r);
}

static void ws_stream_task(void *arg)
{
    (void)arg;
    int frame_count = 0;
    int my_generation;

    xSemaphoreTake(ws_state.lock, portMAX_DELAY);
    my_generation = ws_state.generation;
    xSemaphoreGive(ws_state.lock);

    while (1) {
        xSemaphoreTake(ws_state.lock, portMAX_DELAY);
        bool active = ws_state.active;
        httpd_handle_t hd = ws_state.hd;
        int fd = ws_state.fd;
        int gen = ws_state.generation;
        xSemaphoreGive(ws_state.lock);

        if (!active || gen != my_generation) break;

        camera_frame_t frame = camera_frame_generate(
            motor_left_speed, motor_right_speed, frame_count);

        if (frame.size > 0) {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(ws_pkt));
            ws_pkt.type = HTTPD_WS_TYPE_BINARY;
            ws_pkt.payload = frame.data;
            ws_pkt.len = frame.size;
            if (httpd_ws_send_frame_async(hd, fd, &ws_pkt) != ESP_OK) break;
        }

        char status[64];
        int status_len = snprintf(status, sizeof(status),
            "{\"l\":%" PRId32 ",\"r\":%" PRId32 "}",
            motor_left_speed, motor_right_speed);
        if (status_len > 0 && status_len < (int)sizeof(status)) {
            httpd_ws_frame_t txt_pkt;
            memset(&txt_pkt, 0, sizeof(txt_pkt));
            txt_pkt.type = HTTPD_WS_TYPE_TEXT;
            txt_pkt.payload = (uint8_t *)status;
            txt_pkt.len = status_len;
            if (httpd_ws_send_frame_async(hd, fd, &txt_pkt) != ESP_OK) break;
        }

        if (frame_count % 5 == 0) {
            httpd_ws_frame_t ping_pkt;
            memset(&ping_pkt, 0, sizeof(ping_pkt));
            ping_pkt.type = HTTPD_WS_TYPE_PING;
            if (httpd_ws_send_frame_async(hd, fd, &ping_pkt) != ESP_OK) break;
        }

        frame_count++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "ws stream gen=%d exiting", my_generation);
    vTaskDelete(NULL);
}

static esp_err_t ws_post_handshake_cb(httpd_req_t *req)
{
    xSemaphoreTake(ws_state.lock, portMAX_DELAY);
    ws_state.active = false;
    ws_state.generation++;
    ws_state.hd = req->handle;
    ws_state.fd = httpd_req_to_sockfd(req);
    ws_state.active = true;
    xSemaphoreGive(ws_state.lock);

    BaseType_t ret = xTaskCreate(ws_stream_task, "ws_stream", 4096, NULL, 4, NULL);
    if (ret != pdPASS) {
        xSemaphoreTake(ws_state.lock, portMAX_DELAY);
        ws_state.active = false;
        ws_state.hd = NULL;
        ws_state.fd = -1;
        xSemaphoreGive(ws_state.lock);
        ESP_LOGE(TAG, "ws stream task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ws connected gen=%d", ws_state.generation);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) goto disconnect;

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) goto disconnect;

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.len > 0 && ws_pkt.len < 64) {
        char buf[64];
        ws_pkt.payload = (uint8_t *)buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) goto disconnect;
        buf[ws_pkt.len] = '\0';

        int32_t l = 0, r = 0;
        uint64_t s = 0;
        char *end = NULL;
        l = (int32_t)strtol(buf, &end, 10);
        if (end && *end == ',') {
            r = (int32_t)strtol(end + 1, &end, 10);
            if (end && *end == ',') s = strtoull(end + 1, NULL, 10);
        }
        set_motor(MAX(-255, MIN(255, l)), MAX(-255, MIN(255, r)), s);
    }

    return ESP_OK;

disconnect:
    xSemaphoreTake(ws_state.lock, portMAX_DELAY);
    ws_state.active = false;
    xSemaphoreGive(ws_state.lock);
    ESP_LOGI(TAG, "ws disconnected");
    return ret;
}

static const httpd_uri_t uri_handlers[] = {
    {
        .uri      = "/ws",
        .method   = HTTP_GET,
        .handler  = ws_handler,
        .is_websocket = true,
        .ws_post_handshake_cb = ws_post_handshake_cb,
    },
};

void http_server_start(void)
{
    ws_state.lock = xSemaphoreCreateMutex();
    assert(ws_state.lock);

    camera_frame_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    config.task_priority    = 7;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    for (size_t i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_handlers[i]));
    }

    ESP_LOGI(TAG, "server ready: WS /ws");
}
