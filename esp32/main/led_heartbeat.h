#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void led_heartbeat_init(void);
void led_set_motor(int left, int right);

#ifdef __cplusplus
}
#endif
