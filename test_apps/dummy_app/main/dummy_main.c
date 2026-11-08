/*
 * Basecamp dummy app. Flash three builds into ota_0/1/2 and check:
 *   - launcher tap boots the right slot            (screen shows partition)
 *   - "Restart" / "Crash" / "Sleep 2s" stay here    (boot counter increments)
 *   - power-cycle or RST button returns to launcher
 */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "basecamp_bootsel.h"

static const char *TAG = "dummy";

/* Survives software resets and deep sleep (no init on warm boot). */
static RTC_NOINIT_ATTR uint32_t s_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_boot_count;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "esp_restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_INT_WDT:   return "int wdt";
    case ESP_RST_TASK_WDT:  return "task wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    default:                return "other";
    }
}

static void do_restart(lv_event_t *e)  { board_display_off(); esp_restart(); }
static void do_crash(lv_event_t *e)    { abort(); }
static void do_sleep(lv_event_t *e)
{
    board_display_off();
    esp_sleep_enable_timer_wakeup(2 * 1000 * 1000);
    esp_deep_sleep_start();
}

static lv_obj_t *mk_btn(lv_obj_t *p, const char *txt, int x, int y, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, 120, 64);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

void app_main(void)
{
    if (s_boot_magic != 0xD0D0CAFE) {
        s_boot_magic = 0xD0D0CAFE;
        s_boot_count = 0;
    }
    s_boot_count++;

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *d = esp_app_get_description();
    uint8_t sel = 0xFF;
    bool has_sel = basecamp_bootsel_get(&sel);
    esp_reset_reason_t rr = esp_reset_reason();

    ESP_LOGI(TAG, "%s %s running from %s @0x%06" PRIx32 ", boot #%" PRIu32 ", reset=%s, bootsel=%s%d",
             d->project_name, d->version, run->label, run->address, s_boot_count,
             reset_reason_str(rr), has_sel ? "ota_" : "none/", has_sel ? sel : -1);

    ESP_ERROR_CHECK(board_init(NULL));

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A20), 0);

    lv_obj_t *big = lv_label_create(scr);
    lv_label_set_text_fmt(big, "DUMMY %s", DUMMY_ID);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(big, lv_color_white(), 0);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -120);

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text_fmt(info, "%s @ 0x%06" PRIx32 "\nboot #%" PRIu32 "  (%s)\nbootsel: %s%d",
                          run->label, run->address, s_boot_count, reset_reason_str(rr),
                          has_sel ? "ota_" : "none ", has_sel ? sel : -1);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xC0C0FF), 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -40);

    mk_btn(scr, "Restart", -130, 60, do_restart);
    mk_btn(scr, "Sleep 2s", 0, 60, do_sleep);
    mk_btn(scr, "Crash", 130, 60, do_crash);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "power-cycle / RST = menu");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8080A0), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 150);
    lvgl_port_unlock();
}
