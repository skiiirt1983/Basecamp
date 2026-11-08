/*
 * Basecamp boot-selection self-test (headless, for QEMU or a bare board).
 *
 * factory role (launcher stand-in), step counter kept in NVS:
 *   step 0..2 : select ota_<step>                         -> expect that slot
 *   step 3    : erase ota_2, select ota_2                 -> expect factory (not ota_1!)
 *   step 4    : verify the fall-back, write a corrupt word -> expect factory
 *   step 5    : verify, print SELFTEST PASS
 *
 * ota role, per-slot counter kept in RTC_NOINIT memory:
 *   boot 1 : check slot == selection, esp_restart()        -> expect same slot
 *   boot 2 : abort() (panic reset)                         -> expect same slot
 *   boot 3 : 300 ms timer deep sleep                       -> expect same slot
 *            (only with -DSELFTEST_DEEP_SLEEP=1: QEMU does not emulate the
 *             deep-sleep wake-up, so it is skipped by default)
 *   last   : clear selection, esp_restart()                -> expect factory
 */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "basecamp_bootsel.h"

#ifndef SELFTEST_DEEP_SLEEP
#define SELFTEST_DEEP_SLEEP 0
#endif

static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_count;
static RTC_NOINIT_ATTR uint32_t s_slot;

static void fail(const char *why)
{
    printf("SELFTEST FAIL: %s\n", why);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void select_and_restart(uint8_t slot)
{
    printf("ST: factory selects ota_%u\n", slot);
    fflush(stdout);
    basecamp_bootsel_set(slot);
    esp_restart();
}

static void factory_role(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("selftest", NVS_READWRITE, &h));
    uint8_t step = 0;
    nvs_get_u8(h, "step", &step);
    ESP_ERROR_CHECK(nvs_set_u8(h, "step", step + 1));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);

    uint8_t sel;
    if (basecamp_bootsel_get(&sel)) {
        fail("factory booted while a valid selection is stored");
    }
    printf("ST: factory step %u, reset reason %d\n", step, (int)esp_reset_reason());

    switch (step) {
    case 0: case 1: case 2:
        select_and_restart(step);
        break;
    case 3: {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_OTA_2, NULL);
        ESP_ERROR_CHECK(esp_partition_erase_range(p, 0, 0x10000));
        printf("ST: erased start of ota_2\n");
        select_and_restart(2);
        break;
    }
    case 4:
        printf("ST: empty ota_2 fell back to factory - OK\n");
        REG_WRITE(BASECAMP_BOOTSEL_REG, ((uint32_t)BASECAMP_BOOTSEL_MAGIC << 16) | 0x0001); /* bad check byte */
        printf("ST: wrote corrupt selection word\n");
        fflush(stdout);
        esp_restart();
        break;
    case 5:
        printf("ST: corrupt word fell back to factory - OK\n");
        printf("SELFTEST PASS\n");
        break;
    default:
        printf("ST: idle (step %u)\n", step);
        break;
    }
}

static void ota_role(const esp_partition_t *run)
{
    int me = run->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    uint8_t sel = 0xFF;
    if (!basecamp_bootsel_get(&sel)) {
        fail("ota app running without a valid selection");
    }
    if (sel != me) {
        printf("ST: running ota_%d but selection is ota_%u\n", me, sel);
        fail("booted the wrong slot");
    }
    if (s_magic != 0x5E1F7E57 || s_slot != (uint32_t)me) {
        s_magic = 0x5E1F7E57;
        s_slot = me;
        s_count = 0;
    }
    s_count++;
    printf("ST: ota_%d boot %lu, reset reason %d\n", me, (unsigned long)s_count, (int)esp_reset_reason());
    fflush(stdout);

    switch (s_count) {
    case 1:
        esp_restart();
        break;
    case 2:
        printf("ST: ota_%d panicking on purpose\n", me);
        fflush(stdout);
        abort();
        break;
#if SELFTEST_DEEP_SLEEP
    case 3:
        esp_sleep_enable_timer_wakeup(300 * 1000);
        esp_deep_sleep_start();
        break;
#endif
    default:
        printf("ST: ota_%d stayed put through restart/panic%s - OK\n", me,
               SELFTEST_DEEP_SLEEP ? "/deep-sleep" : "");
        s_magic = 0;
        basecamp_bootsel_clear();
        fflush(stdout);
        esp_restart();
        break;
    }
}

void app_main(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    printf("ST: running from %s @0x%06lx\n", run->label, (unsigned long)run->address);
    if (run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        factory_role();
    } else {
        ota_role(run);
    }
}
