/* LVGL lock used by ui.c. On the device it is the esp_lvgl_port lock; the
 * PC preview build (tools/ui_preview) defines UI_HOST to make it a no-op. */
#pragma once
#ifdef UI_HOST
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#else
#include "esp_lvgl_port.h"
#define LOCK()   lvgl_port_lock(0)
#define UNLOCK() lvgl_port_unlock()
#endif
