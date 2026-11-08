/*
 * SLOPEFORM / Project Basecamp - boot slot selection contract.
 *
 * Shared between the custom 2nd-stage bootloader (bootloader_components/main)
 * and the launcher / test apps. Header-only, no IDF component dependencies
 * beyond register access, so it can be compiled into the bootloader.
 *
 * Storage: RTC_CNTL_STORE0_REG (ESP32-S3).
 *   - ROM header (components/esp_rom/esp32s3/include/esp32s3/rom/rtc.h) lists
 *     STORE0 as "Reserved"; STORE1..7 are used by ROM/IDF (slow clk cal, boot
 *     time, xtal freq, wake stub, reset cause, RTC mem CRC).
 *   - grep of ESP-IDF v5.5 shows no use of STORE0 outside ULP unit tests.
 *   - Lives in the RTC domain: survives software reset, panic / watchdog
 *     system resets and deep sleep; is cleared by power-on / chip reset.
 *
 * Encoding (32 bit):
 *   [31:16] BASECAMP_BOOTSEL_MAGIC
 *   [15: 8] ~slot (one's complement check byte)
 *   [ 7: 0] slot  (0 = ota_0, 1 = ota_1, 2 = ota_2)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BASECAMP_BOOTSEL_REG        RTC_CNTL_STORE0_REG
#define BASECAMP_BOOTSEL_MAGIC      0xBA5Eu
#define BASECAMP_NUM_SLOTS          3

static inline uint32_t basecamp_bootsel_encode(uint8_t slot)
{
    return ((uint32_t)BASECAMP_BOOTSEL_MAGIC << 16) |
           ((uint32_t)(uint8_t)~slot << 8) |
           (uint32_t)slot;
}

/* Returns true and writes *slot if the register holds a valid selection. */
static inline bool basecamp_bootsel_decode(uint32_t v, uint8_t *slot)
{
    uint8_t s = (uint8_t)(v & 0xFF);
    uint8_t chk = (uint8_t)((v >> 8) & 0xFF);
    if ((v >> 16) != BASECAMP_BOOTSEL_MAGIC || chk != (uint8_t)~s || s >= BASECAMP_NUM_SLOTS) {
        return false;
    }
    *slot = s;
    return true;
}

static inline void basecamp_bootsel_set(uint8_t slot)
{
    REG_WRITE(BASECAMP_BOOTSEL_REG, basecamp_bootsel_encode(slot));
}

static inline void basecamp_bootsel_clear(void)
{
    REG_WRITE(BASECAMP_BOOTSEL_REG, 0);
}

static inline bool basecamp_bootsel_get(uint8_t *slot)
{
    return basecamp_bootsel_decode(REG_READ(BASECAMP_BOOTSEL_REG), slot);
}

#ifdef __cplusplus
}
#endif
