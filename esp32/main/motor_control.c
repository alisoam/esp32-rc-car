#include "motor_control.h"
#include "led_heartbeat.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "motor"

#define MOTOR_PWM_FREQ_HZ    5000
#define MOTOR_PWM_RESOLUTION LEDC_TIMER_10_BIT
#define MOTOR_PWM_TIMER      LEDC_TIMER_0
#define MOTOR_PWM_MODE       LEDC_LOW_SPEED_MODE

#define MOTOR_LEFT_FWD_GPIO    4
#define MOTOR_LEFT_REV_GPIO    5
#define MOTOR_RIGHT_FWD_GPIO   6
#define MOTOR_RIGHT_REV_GPIO   7

#define MOTOR_CH_LEFT_FWD      LEDC_CHANNEL_0
#define MOTOR_CH_LEFT_REV      LEDC_CHANNEL_1
#define MOTOR_CH_RIGHT_FWD     LEDC_CHANNEL_2
#define MOTOR_CH_RIGHT_REV     LEDC_CHANNEL_3

#define MOTOR_DEADBAND         20
#define MOTOR_RAMP_LIMIT       50
#define MOTOR_WATCHDOG_MS      1500
#define MOTOR_MAX_SPEED        255
#define MOTOR_MAX_DUTY         1023

static SemaphoreHandle_t motor_mutex = NULL;
static int      cur_left       = 0;
static int      cur_right      = 0;
static uint64_t last_command_us = 0;

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int apply_ramp(int target, int current)
{
    int diff = target - current;
    if (diff >  MOTOR_RAMP_LIMIT) diff =  MOTOR_RAMP_LIMIT;
    if (diff < -MOTOR_RAMP_LIMIT) diff = -MOTOR_RAMP_LIMIT;
    return current + diff;
}

/* Caller must hold motor_mutex. */
static void drive_motor_locked(int speed,
                               ledc_channel_t fwd_ch,
                               ledc_channel_t rev_ch)
{
    uint32_t fwd_duty = 0;
    uint32_t rev_duty = 0;

    if (speed > MOTOR_DEADBAND) {
        fwd_duty = (uint32_t)(speed * MOTOR_MAX_DUTY) / MOTOR_MAX_SPEED;
    } else if (speed < -MOTOR_DEADBAND) {
        rev_duty = (uint32_t)(-speed * MOTOR_MAX_DUTY) / MOTOR_MAX_SPEED;
    }

    ledc_set_duty(MOTOR_PWM_MODE, fwd_ch, fwd_duty);
    ledc_update_duty(MOTOR_PWM_MODE, fwd_ch);
    ledc_set_duty(MOTOR_PWM_MODE, rev_ch, rev_duty);
    ledc_update_duty(MOTOR_PWM_MODE, rev_ch);
}

static void drive_all_locked(int left, int right)
{
    drive_motor_locked(left,  MOTOR_CH_LEFT_FWD,  MOTOR_CH_LEFT_REV);
    drive_motor_locked(right, MOTOR_CH_RIGHT_FWD, MOTOR_CH_RIGHT_REV);
}

void motor_set(int left, int right)
{
    left  = clamp_int(left,  -MOTOR_MAX_SPEED, MOTOR_MAX_SPEED);
    right = clamp_int(right, -MOTOR_MAX_SPEED, MOTOR_MAX_SPEED);
    if (left  > -MOTOR_DEADBAND && left  < MOTOR_DEADBAND) left  = 0;
    if (right > -MOTOR_DEADBAND && right < MOTOR_DEADBAND) right = 0;

    int new_left, new_right;
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    new_left  = apply_ramp(left,  cur_left);
    new_right = apply_ramp(right, cur_right);
    cur_left       = new_left;
    cur_right      = new_right;
    last_command_us = esp_timer_get_time();
    drive_all_locked(new_left, new_right);
    xSemaphoreGive(motor_mutex);

    led_set_motor(new_left, new_right);
}

void motor_stop(void)
{
    xSemaphoreTake(motor_mutex, portMAX_DELAY);
    cur_left        = 0;
    cur_right       = 0;
    last_command_us = esp_timer_get_time();
    drive_all_locked(0, 0);
    xSemaphoreGive(motor_mutex);
    led_set_motor(0, 0);
}

static void motor_watchdog_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        int stop_needed = 0;
        uint64_t elapsed_ms = 0;

        xSemaphoreTake(motor_mutex, portMAX_DELAY);
        elapsed_ms = (esp_timer_get_time() - last_command_us) / 1000;
        if (elapsed_ms > MOTOR_WATCHDOG_MS &&
            (cur_left != 0 || cur_right != 0)) {
            stop_needed = 1;
            cur_left  = 0;
            cur_right = 0;
            drive_all_locked(0, 0);
        }
        xSemaphoreGive(motor_mutex);

        if (stop_needed) {
            ESP_LOGW(TAG, "watchdog: no command for %llu ms, stopping",
                     elapsed_ms);
            led_set_motor(0, 0);
        }
    }
}

void motor_control_init(void)
{
    motor_mutex = xSemaphoreCreateMutex();
    assert(motor_mutex != NULL);

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = MOTOR_PWM_MODE,
        .duty_resolution = MOTOR_PWM_RESOLUTION,
        .timer_num       = MOTOR_PWM_TIMER,
        .freq_hz         = MOTOR_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const struct {
        int gpio;
        ledc_channel_t ch;
    } channels[] = {
        { MOTOR_LEFT_FWD_GPIO,  MOTOR_CH_LEFT_FWD  },
        { MOTOR_LEFT_REV_GPIO,  MOTOR_CH_LEFT_REV  },
        { MOTOR_RIGHT_FWD_GPIO, MOTOR_CH_RIGHT_FWD },
        { MOTOR_RIGHT_REV_GPIO, MOTOR_CH_RIGHT_REV },
    };

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        ledc_channel_config_t ch_cfg = {
            .gpio_num   = channels[i].gpio,
            .speed_mode = MOTOR_PWM_MODE,
            .channel    = channels[i].ch,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = MOTOR_PWM_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    }

    last_command_us = esp_timer_get_time();

    BaseType_t ret = xTaskCreatePinnedToCore(
        motor_watchdog_task,
        "motor_wdog",
        2048,
        NULL,
        1,
        NULL,
        0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create motor watchdog task");
    } else {
        ESP_LOGI(TAG,
                 "motor control ready: PWM %d Hz %d-bit, deadband +/-%d, "
                 "ramp +/-%d, watchdog %d ms",
                 MOTOR_PWM_FREQ_HZ, MOTOR_MAX_DUTY + 1,
                 MOTOR_DEADBAND, MOTOR_RAMP_LIMIT, MOTOR_WATCHDOG_MS);
    }
}
