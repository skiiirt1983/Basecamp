/*
 * SLOPEFORM / Project Basecamp - factory launcher.
 *
 * Shows the three OTA slots; tapping an installed one stores the selection in
 * RTC_CNTL_STORE0_REG and restarts (the custom bootloader then boots that slot
 * until the next cold reset). Empty slots are installed straight from the
 * apps' GitHub releases; on a fresh install (all slots empty) this starts
 * automatically, with a captive Wi-Fi setup portal if no Wi-Fi is stored.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "board.h"
#include "axp2101.h"
#include "basecamp_bootsel.h"
#include "slots.h"
#include "ui.h"
#include "netmgr.h"
#include "installer.h"

static const char *TAG = "launcher";
static basecamp_slot_t s_slots[BASECAMP_NUM_SLOTS];
static QueueHandle_t s_jobs;
static volatile bool s_busy;

typedef struct {
    ui_req_t req;
    int arg;
} job_t;

#define ALL_SLOTS ((1u << BASECAMP_NUM_SLOTS) - 1)
#define INSTALL_ATTEMPTS 4

static void restart_to_menu(void)
{
    net_shutdown();
    board_display_off();
    basecamp_bootsel_clear();
    esp_restart();
}

/* UI requests arrive on the LVGL task: hand them to the worker. */
static void on_request(ui_req_t req, int arg)
{
    if (req == UI_REQ_RESTART) {
        restart_to_menu(); /* also cancels anything in progress */
    }
    if (s_busy) {
        return;
    }
    s_busy = true;
    job_t j = {req, arg};
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        s_busy = false;
    }
}

/* ---------- network gate ---------- */

static bool ensure_network(void)
{
    char notice[96] = "";
    if (net_init() != ESP_OK) {
        ui_status("Wi-Fi failed to start");
        return false;
    }
    if (net_has_credentials()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to %s ...", net_ssid());
        ui_status(msg);
        if (net_connect(20000)) {
            return true;
        }
        snprintf(notice, sizeof(notice), "Could not connect to \"%s\". Choose a network again.", net_ssid());
    }
    ui_show_wifi_setup(notice[0] ? notice : NULL);
    if (net_portal_start() != ESP_OK) {
        ui_status("Could not start the setup network");
        return false;
    }
    net_portal_wait(UINT32_MAX);        /* Cancel restarts the launcher */
    vTaskDelay(pdMS_TO_TICKS(2500));    /* let the phone receive the "Connected" page */
    net_portal_stop();
    return net_is_connected() || net_connect(20000);
}

/* ---------- install ---------- */

static void progress_cb(int slot, int pct, void *ctx)
{
    ui_progress_slot(slot, pct, (const char *)ctx, false);
}

static void do_install(uint32_t mask, bool update)
{
    const char *ttl = update ? "UPDATING" : "INSTALLING";
    ui_show_progress(ttl, mask);
    if (!ensure_network()) {
        ui_action_button("Menu", UI_REQ_RESTART, 0);
        return;
    }
    ui_show_progress(ttl, mask); /* the Wi-Fi portal may have replaced it */
    ui_status(update ? "Updates keep each app's settings." : "Downloading from GitHub ...");

    uint32_t failed = 0;
    char first_err[160] = "";
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        if (!((mask >> i) & 1)) {
            continue;
        }
        char err[96] = "";
        static inst_release_t rel;
        esp_err_t e = ESP_FAIL;
        /* GitHub hiccups (5xx, timeouts, dropped connections) are retried
         * automatically; wrong app / board / size errors are not. */
        for (int attempt = 1; attempt <= INSTALL_ATTEMPTS; attempt++) {
            char state[48];
            if (attempt > 1) {
                snprintf(state, sizeof(state), "retry %d/%d in %ds", attempt, INSTALL_ATTEMPTS, 3 * (attempt - 1));
                ui_progress_slot(i, 0, state, false);
                vTaskDelay(pdMS_TO_TICKS(3000 * (attempt - 1)));
            }
            ui_progress_slot(i, 0, "checking GitHub", false);
            e = inst_fetch_latest(i, &rel, err, sizeof(err));
            if (e == ESP_OK) {
                snprintf(state, sizeof(state), "downloading %s", rel.tag);
                ui_progress_slot(i, 0, state, false);
                e = inst_install(i, &rel, progress_cb, state, err, sizeof(err));
            }
            if (e != ESP_ERR_TIMEOUT) {
                break;
            }
        }
        if (e == ESP_OK) {
            ui_progress_slot(i, 100, "installed", false);
        } else {
            failed |= 1u << i;
            ui_progress_slot(i, -1, err, true);
            if (!first_err[0]) {
                snprintf(first_err, sizeof(first_err), "%.31s: %.95s", s_slots[i].name, err);
            }
        }
    }

    if (!failed) {
        ui_status("All done - back to the menu");
        vTaskDelay(pdMS_TO_TICKS(1500));
        restart_to_menu();
    }
    ui_status(first_err);
    ui_action_pair("Retry", UI_REQ_INSTALL, failed, "Menu", UI_REQ_RESTART, 0);
}

/* ---------- updates ---------- */

static const char *strip_v(const char *s)
{
    return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
}

static void do_check_updates(void)
{
    ui_show_updates();
    if (!ensure_network()) {
        ui_action_button("Back", UI_REQ_RESTART, 0);
        return;
    }
    ui_show_updates();
    ui_status("Checking GitHub releases ...");
    uint32_t avail = 0;
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        const basecamp_slot_t *s = &s_slots[i];
        static inst_release_t rel;
        char err[96], row[140];
        esp_err_t fe = ESP_ERR_TIMEOUT;
        for (int attempt = 0; attempt < INSTALL_ATTEMPTS && fe == ESP_ERR_TIMEOUT; attempt++) {
            if (attempt) {
                vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
            }
            fe = inst_fetch_latest(i, &rel, err, sizeof(err));
        }
        if (fe != ESP_OK) {
            snprintf(row, sizeof(row), "check failed: %.95s", err);
            ui_updates_row(i, row, false);
            continue;
        }
        /* Installed version: the tag the launcher installed, else the
         * descriptor version (not meaningful for generic Arduino builds). */
        const char *cur = s->tag[0] ? s->tag : (s->valid && !s->generic_desc ? s->desc.version : "");
        bool up_to_date = s->valid && cur[0] && strcmp(strip_v(cur), strip_v(rel.tag)) == 0;
        if (!s->valid) {
            snprintf(row, sizeof(row), "not installed -> %s", rel.tag);
        } else if (up_to_date) {
            snprintf(row, sizeof(row), "%s  (up to date)", cur);
        } else {
            snprintf(row, sizeof(row), "%s -> %s", cur[0] ? cur : "?", rel.tag);
        }
        if (!up_to_date) {
            avail |= 1u << i;
        }
        ui_updates_row(i, row, !up_to_date);
    }
    if (avail) {
        ui_status("Install updates? App settings are kept.");
        ui_action_pair("Install", UI_REQ_INSTALL_UPDATES, avail, "Back", UI_REQ_RESTART, 0);
    } else {
        ui_status("Everything is up to date.");
        ui_action_button("Back", UI_REQ_RESTART, 0);
    }
}

/* ---------- worker ---------- */

static void launch(int slot)
{
    ESP_LOGI(TAG, "launching ota_%d (%s %s)", slot, s_slots[slot].name, s_slots[slot].version);
    ui_show_launching(slot);
    vTaskDelay(pdMS_TO_TICKS(250));
    net_shutdown();
    board_display_off();
    basecamp_bootsel_set((uint8_t)slot);
    esp_restart();
}

static void worker(void *arg)
{
    job_t j;
    for (;;) {
        xQueueReceive(s_jobs, &j, portMAX_DELAY);
        switch (j.req) {
        case UI_REQ_LAUNCH:
            launch(j.arg);
            break;
        case UI_REQ_INSTALL:
            do_install((uint32_t)j.arg & ALL_SLOTS, false);
            break;
        case UI_REQ_INSTALL_UPDATES:
            do_install((uint32_t)j.arg & ALL_SLOTS, true);
            break;
        case UI_REQ_CHECK_UPDATES:
            do_check_updates();
            break;
        case UI_REQ_ACCEPT:
            slots_accept(&s_slots[j.arg], j.arg);
            ui_menu_refresh_slot(j.arg);
            s_busy = false;
            break;
        default:
            break;
        }
        /* Install / update screens end with Retry / Menu / Back buttons. */
        s_busy = false;
    }
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
        ui_set_footer(buf);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    /* The launcher only runs when nothing valid is selected; never leave a
     * stale selection behind. */
    basecamp_bootsel_clear();
    ESP_LOGI(TAG, "Basecamp launcher %s", esp_app_get_description()->version);

    /* NVS is shared with the apps: never erase it here. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s - settings and overwrite guard unavailable", esp_err_to_name(err));
    }

    slots_scan(s_slots);
    ESP_ERROR_CHECK(board_init(NULL));

    s_jobs = xQueueCreate(2, sizeof(job_t));
    ui_init(s_slots, on_request);
    ui_show_menu();
    xTaskCreate(worker, "worker", 10240, NULL, 5, NULL);
    xTaskCreate(footer_task, "footer", 3072, NULL, 2, NULL);

    bool any = false;
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        any |= s_slots[i].valid;
    }
    if (!any) {
        ESP_LOGI(TAG, "no apps installed - starting first-time setup");
        on_request(UI_REQ_INSTALL, ALL_SLOTS);
    }
}
