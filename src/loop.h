#ifndef KCPUV_LOOP_H
#define KCPUV_LOOP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "uv.h"

uv_loop_t *kcpuv_get_loop();

void kcpuv_use_default_loop(int value);

void kcpuv__add_idle(uv_idle_t *idle);

void kcpuv_start_loop(uv_timer_cb cb);

int kcpuv_stop_loop();

void kcpuv__destroy_loop();

#ifdef __cplusplus
}
#endif

#endif
