#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t   size;
} ov7670_frame_t;

esp_err_t ov7670_frame_init(void);
void ov7670_frame_deinit(void);

ov7670_frame_t ov7670_frame_get(void);

#ifdef __cplusplus
}
#endif
