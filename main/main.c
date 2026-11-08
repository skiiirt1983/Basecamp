/*
 * SLOPEFORM / Project Basecamp - factory launcher.
 *
 * Shows the three OTA slots; tapping one stores the selection in
 * RTC_CNTL_STORE0_REG and restarts. The custom bootloader then boots that
 * slot until the next cold reset (power cycle / RST button).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "axp2101.h"
#include "basecamp_bootsel.h"
#include "slots.h"
#include "ui.h"

static const char *TAG = "launcher";
static basecamp_slot_t s_slots[BASECAMP_NUM_SLOTS];

static void launch(int slot)
{
    ESP_LOGI(TAG, "launching ota_%d (%s %s)", slot, s_slots[slot].name, s_slots[slot].version);
    board_display_off();
    basecamp_bootsel_set((uint8_t)slot);
    esp_restart();
}

static void accept(int slot)
{
    slots_accept(&s_slots[slot], slot);
}

static void footer_task(void *arg)
{
    char buf[48];
    for (;;) {
        int pct = axp2101_battery_percent();
        bool usb = axp2101_vbus_present();
        if (pct >= 0) {
            snprintf(buf, sizeof(buf), "%s%d%%", usb ? "USB  " : "", pct);
        } else {
            snprintf(buf, sizeof(buf), "%s", usb ? "USB" : "");
        }
        if (lvgl_port_lock(100)) {
            ui_set_footer(buf);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    /* Belt and braces: the launcher only runs when nothing valid is selected,
     * but never leave a stale selection behind. */
    basecamp_bootsel_clear();

    ESP_LOGI(TAG, "Basecamp launcher %s", esp_app_get_description()->version);

    /* NVS is shared with the apps: never erase it here, just skip the
     * overwrite guard if it can't be opened. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s - overwrite guard disabled", esp_err_to_name(err));
    }

    slots_scan(s_slots);

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_init(&disp));

    if (lvgl_port_lock(0)) {
        ui_create(s_slots, launch, accept);
        lvgl_port_unlock();
    }
    xTaskCreate(footer_task, "footer", 3072, NULL, 2, NULL);
}
