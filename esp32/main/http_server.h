#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int32_t  motor_left_speed;
extern volatile int32_t  motor_right_speed;
extern volatile uint64_t last_control_seq;
extern volatile int      stream_clients;

void http_server_start(void);

#ifdef __cplusplus
}
#endif
