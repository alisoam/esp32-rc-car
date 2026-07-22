#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OV7670_D0     GPIO_NUM_11
#define OV7670_D1     GPIO_NUM_12
#define OV7670_D2     GPIO_NUM_13
#define OV7670_D3     GPIO_NUM_14
#define OV7670_D4     GPIO_NUM_15
#define OV7670_D5     GPIO_NUM_16
#define OV7670_D6     GPIO_NUM_17
#define OV7670_D7     GPIO_NUM_18

#define OV7670_PCLK   GPIO_NUM_9
#define OV7670_VSYNC  GPIO_NUM_10
#define OV7670_HREF   GPIO_NUM_3
#define OV7670_XCLK   GPIO_NUM_21

#define OV7670_SCL    GPIO_NUM_1
#define OV7670_SDA    GPIO_NUM_2

#define OV7670_WIDTH   176
#define OV7670_HEIGHT  144

esp_err_t ov7670_init(void);

#ifdef __cplusplus
}
#endif
