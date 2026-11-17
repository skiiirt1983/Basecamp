/*
 * Basecamp - custom 2nd-stage bootloader entry point.
 *
 * Derived from ESP-IDF v5.5 components/bootloader/subproject/main/bootloader_start.c
 * (Apache-2.0, Espressif Systems). Only the partition selection differs:
 *
 *   - otadata is ignored for selection.
 *   - On a cold reset (power-on, EN/RST button, brownout, RTC WDT and, by
 *     default, USB-Serial-JTAG chip reset) the selection register is cleared
 *     and the factory launcher is booted.
 *   - Otherwise, if RTC_CNTL_STORE0_REG holds a valid selection word written
 *     by the launcher, that OTA slot is verified and booted. The word is left
 *     in place so the app's own restarts / deep-sleep wakeups stay in the app.
 *   - Anything else (no word, bad word, empty / corrupt slot) -> launcher.
 */
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "esp_image_format.h"
#include "basecamp_bootsel.h"

static const char *TAG = "basecamp";

static bool is_cold_reset(soc_reset_reason_t r)
{
    switch (r) {
    case RESET_REASON_CHIP_POWER_ON:    /* power-on and EN (RST button) */
    case RESET_REASON_SYS_BROWN_OUT:
    case RESET_REASON_SYS_RTC_WDT:
    case RESET_REASON_SYS_CLK_GLITCH:
    case RESET_REASON_CORE_PWR_GLITCH:
    case RESET_REASON_CORE_EFUSE_CRC:
        return true;
#if CONFIG_BASECAMP_USB_RESET_IS_COLD
    case RESET_REASON_CORE_USB_UART:
    case RESET_REASON_CORE_USB_JTAG:
        return true;
#endif
    default:
        return false;
    }
}

static bool slot_is_bootable(const esp_partition_pos_t *pos)
{
    if (pos->offset == 0 || pos->size == 0) {
        return false;
    }
    esp_image_metadata_t md = {0};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, pos, &md) == ESP_OK;
}

static int basecamp_select(const bootloader_state_t *bs)
{
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    uint32_t raw = REG_READ(BASECAMP_BOOTSEL_REG);
    uint8_t slot = 0;

    if (is_cold_reset(reason)) {
        ESP_LOGI(TAG, "cold reset (reason %d): launcher", (int)reason);
        basecamp_bootsel_clear();
        return FACTORY_INDEX;
    }
    if (!basecamp_bootsel_decode(raw, &slot)) {
        ESP_LOGI(TAG, "no slot selected (0x%08" PRIx32 ", reason %d): launcher", raw, (int)reason);
        basecamp_bootsel_clear();
        return FACTORY_INDEX;
    }
    if (slot >= bs->app_count) {
        ESP_LOGE(TAG, "slot %u not in partition table: launcher", slot);
        basecamp_bootsel_clear();
        return FACTORY_INDEX;
    }
    if (!slot_is_bootable(&bs->ota[slot])) {
        /* bootloader_utility_load_boot_image() would otherwise fall back to
         * *other* OTA slots before the factory app - never boot the wrong app. */
        ESP_LOGE(TAG, "ota_%u @0x%" PRIx32 " is empty or invalid: launcher",
                 slot, bs->ota[slot].offset);
        basecamp_bootsel_clear();
        return FACTORY_INDEX;
    }
    ESP_LOGI(TAG, "booting ota_%u @0x%" PRIx32 " (reason %d)", slot, bs->ota[slot].offset, (int)reason);
    return slot; /* OTA slot index n == ota_n */
}

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        bootloader_reset();
    }
    if (bs.factory.offset == 0) {
        ESP_LOGE(TAG, "no factory (launcher) partition!");
        bootloader_reset();
    }

    int boot_index = basecamp_select(&bs);

    /* Never returns. On a load failure of the chosen index it walks down to
     * FACTORY_INDEX (the slot was verified above, so this is a last resort). */
    bootloader_utility_load_boot_image(&bs, boot_index);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
