#include "led_heartbeat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "heartbeat";

static void heartbeat_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "tick");
    }
}

void led_heartbeat_init(void)
{
    BaseType_t ret = xTaskCreate(
        heartbeat_task,
        "heartbeat",
        2048,
        NULL,
        1,
        NULL
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task");
    } else {
        ESP_LOGI(TAG, "Heartbeat task started");
    }
}
