#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_ap.h"
#include "http_server.h"
#include "led_heartbeat.h"
#include "motor_control.h"
#include "camera_frame.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32 RC Car firmware starting...");

    led_heartbeat_init();
    motor_control_init();
    camera_frame_init_real();

    wifi_ap_init();
    http_server_start();

    ESP_LOGI(TAG, "Firmware ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
