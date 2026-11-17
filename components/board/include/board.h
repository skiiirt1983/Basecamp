#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 466
#define BOARD_LCD_V_RES 466

/* Call before board_init(): start with the panel dark (no white flash from its random RAM at
 * power-up). Then call board_set_brightness() once the first screen has been drawn. */
void board_set_dark_start(bool dark);

/* Bring up I2C, AXP2101 rails, CO5300 AMOLED, CST9217 touch and LVGL.
 * After this, wrap LVGL calls in lvgl_port_lock()/lvgl_port_unlock(). */
esp_err_t board_init(lv_display_t **out_disp);

/* Panel brightness 0..100 %. */
void board_set_brightness(uint8_t percent);

/* Blank the panel (call before esp_restart() for a clean hand-off). */
void board_display_off(void);
