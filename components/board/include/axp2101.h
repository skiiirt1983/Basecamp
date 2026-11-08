#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define AXP2101_I2C_ADDR 0x34

/*
 * Minimal AXP2101 support for the Waveshare ESP32-S3-Touch-AMOLED-1.75.
 * Rail map (from the board schematic):
 *   DCDC1 -> VCC3V3   (ESP32-S3, flash, PSRAM, peripherals)
 *   ALDO1 -> VL1_3.3V
 *   ALDO2 -> VL2_3.3V
 * Other rails are left at their OTP defaults.
 */
esp_err_t axp2101_init(i2c_master_bus_handle_t bus);
/* Battery state of charge in percent, or -1 if no battery / unknown. */
int axp2101_battery_percent(void);
bool axp2101_vbus_present(void);
