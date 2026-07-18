#include "http_server.h"

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
#include <errno.h>

#define STREAM_BOUNDARY "FRAME"

volatile int32_t  motor_left_speed  = 0;
volatile int32_t  motor_right_speed = 0;
volatile uint64_t last_control_seq  = 0;
volatile int      stream_clients    = 0;

static const char *TAG = "http_server";
static int64_t     start_time_us = 0;

static esp_err_t status_handler(httpd_req_t *req)
{
    int64_t uptime = (esp_timer_get_time() - start_time_us) / 1000000;
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "{\"fps\":0,\"clients\":%d,\"uptime\":%" PRId64 ",\"left\":%" PRId32 ",\"right\":%" PRId32 ",\"resolution\":\"176x144\"}",
        stream_clients, uptime, motor_left_speed, motor_right_speed);
    if (n < 0 || n >= sizeof(resp)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t control_handler(httpd_req_t *req)
{
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing params");
        return ESP_FAIL;
    }

    char param[32];
    int32_t  l = 0;
    int32_t  r = 0;
    uint64_t s = 0;

    if (httpd_query_key_value(buf, "l", param, sizeof(param)) == ESP_OK) {
        l = atoi(param);
    }
    if (httpd_query_key_value(buf, "r", param, sizeof(param)) == ESP_OK) {
        r = atoi(param);
    }
    if (httpd_query_key_value(buf, "s", param, sizeof(param)) == ESP_OK) {
        errno = 0;
        s = strtoull(param, NULL, 10);
        if (errno == ERANGE) {
            s = 0;
        }
    }

    l = MAX(-255, MIN(255, l));
    r = MAX(-255, MIN(255, r));

    if (s != 0 && s <= last_control_seq) {
        ESP_LOGW(TAG, "stale seq=%llu dropped", s);
        httpd_resp_sendstr(req, "stale");
        return ESP_OK;
    }
    if (s != 0) {
        last_control_seq = s;
    }
    motor_left_speed  = l;
    motor_right_speed = r;

    ESP_LOGI(TAG, "control: L=%" PRId32 " R=%" PRId32 " s=%llu", l, r, s);

    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    const char *heartbeat =
        "--" STREAM_BOUNDARY "\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    size_t hb_len = strlen(heartbeat);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t err = httpd_resp_send_chunk(req, heartbeat, hb_len);
        if (err != ESP_OK) {
            break;
        }
    }

    httpd_req_async_handler_complete(req);
    stream_clients--;
    ESP_LOGI(TAG, "stream client disconnected (total: %d)", stream_clients);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    stream_clients++;

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "stream client connected (total: %d)", stream_clients);

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "async handler begin failed");
        stream_clients--;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(stream_task, "stream", 4096, async_req, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create stream task");
        stream_clients--;
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static const httpd_uri_t uri_handlers[] = {
    {
        .uri      = "/status",
        .method   = HTTP_GET,
        .handler  = status_handler,
    },
    {
        .uri      = "/control",
        .method   = HTTP_GET,
        .handler  = control_handler,
    },
    {
        .uri      = "/stream",
        .method   = HTTP_GET,
        .handler  = stream_handler,
    },
};

void http_server_start(void)
{
    start_time_us = esp_timer_get_time();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    for (size_t i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_handlers[i]));
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
