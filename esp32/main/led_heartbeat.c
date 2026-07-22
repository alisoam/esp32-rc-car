#include "led_heartbeat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"

#define TAG "heartbeat"

#define RGB_LED_GPIO         48
#define RGB_LED_BRIGHTNESS   32

static led_strip_handle_t s_strip;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void led_set_motor(int left, int right)
{
    int abs_l = left < 0 ? -left : left;
    int abs_r = right < 0 ? -right : right;
    int max_speed = abs_l > abs_r ? abs_l : abs_r;

    uint8_t green = 0;
    uint8_t blue  = 0;

    if (max_speed > 0) {
        uint8_t brightness = (uint8_t)(max_speed * RGB_LED_BRIGHTNESS / 255);
        if (left > 0 || right > 0) green = brightness;
        if (left < 0 || right < 0) blue  = brightness;
    }

    set_rgb(0, green, blue);
}

static void heartbeat_task(void *arg)
{
    (void)arg;

    while (1) {
        for (int i = 0; i < 2; i++) {
            set_rgb(RGB_LED_BRIGHTNESS, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            set_rgb(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(5000 - 2 * 200));
    }
}

void led_heartbeat_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);

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
        ESP_LOGI(TAG, "Heartbeat: 3 fast blinks every 5s (GPIO%d)", RGB_LED_GPIO);
    }
}
