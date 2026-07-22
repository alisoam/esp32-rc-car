#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t   size;
} camera_frame_t;

void camera_frame_init(void);
void camera_frame_init_real(void);
void camera_frame_deinit(void);
camera_frame_t camera_frame_generate(int32_t left_motor, int32_t right_motor, int frame_count);

#ifdef __cplusplus
}
#endif
