/*
 * Basecamp - factory launcher.
 *
 * The menu shows whatever is installed in the three app slots (ota_0..2), in
 * the order chosen on the web page. Tapping an app stores the selection in
 * RTC_CNTL_STORE0_REG and restarts: the custom bootloader then boots that slot
 * until the next cold reset.
 *
 * While the menu is up, an open Wi-Fi "BaseCamp-Setup" serves a web page
 * (webui.c) to upload / order / rename / remove apps. Apps can also be
 * installed from GitHub releases (installer.c) over the home Wi-Fi.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "board.h"
#include "axp2101.h"
#include "basecamp_bootsel.h"
#include "slots.h"
#include "meta.h"
#include "ui.h"
#include "netmgr.h"
#include "webui.h"
#include "installer.h"
#include "ctl.h"

static const char *TAG = "launcher";

#define INSTALL_ATTEMPTS 4

typedef struct {
    int slot;
    inst_source_t src;
    char name[32];
    uint32_t color;
} inst_job_t;

typedef enum { W_REQ, W_BATCH } work_kind_t;
typedef struct {
    work_kind_t kind;
    ui_req_t req;
    int arg;
    bool update_mode;
} work_t;

static QueueHandle_t s_work;
static volatile bool s_busy;
static inst_job_t s_batch[APPS_MAX];
static int s_nbatch;
static inst_job_t s_failed[APPS_MAX];
static int s_nfailed;
static ctl_job_t s_job;
static SemaphoreHandle_t s_job_mtx;
static char s_footer[64];
static int s_up_slot = -1;

/* ---------- why did we (re)start? ---------- */

static char s_boot_text[80];
static bool s_abnormal_reset;

static const char *reason_name(int r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "reset button";
    case ESP_RST_SW:        return "restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brown-out";
    case ESP_RST_USB:       return "USB";
    default:                return "other";
    }
}

/* Keep this start's reason and the previous one in NVS: a quick reset right after power-on then shows
 * up on the web page ("brown-out", "crash", ...) instead of being a mystery. */
static void record_boot(void)
{
    int cur = esp_reset_reason();
    uint8_t prev = 255;
    nvs_handle_t h;
    if (nvs_open("basecamp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u8(h, "rst0", &prev);
        nvs_set_u8(h, "rst0", (uint8_t)cur);
        nvs_commit(h);
        nvs_close(h);
    }
    if (prev == 255) {
        snprintf(s_boot_text, sizeof(s_boot_text), "%s", reason_name(cur));
    } else {
        snprintf(s_boot_text, sizeof(s_boot_text), "%s (before: %s)", reason_name(cur), reason_name(prev));
    }
    s_abnormal_reset = cur == ESP_RST_BROWNOUT || cur == ESP_RST_PANIC || cur == ESP_RST_INT_WDT ||
                       cur == ESP_RST_TASK_WDT || cur == ESP_RST_WDT;
    ESP_LOGI(TAG, "start: %s", s_boot_text);
}

const char *ctl_boot_info(void)
{
    return s_boot_text;
}

/* ---------- busy state / job status (ctl.h) ---------- */

bool ctl_begin(void)
{
    return !__atomic_exchange_n(&s_busy, true, __ATOMIC_ACQ_REL);
}

void ctl_end(void)
{
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
}

bool ctl_busy(void)
{
    return s_busy;
}

void ctl_job_get(ctl_job_t *out)
{
    xSemaphoreTake(s_job_mtx, portMAX_DELAY);
    *out = s_job;
    xSemaphoreGive(s_job_mtx);
}

static void job_begin(void)
{
    xSemaphoreTake(s_job_mtx, portMAX_DELAY);
    s_job.active = true;
    s_job.slot = -1;
    s_job.pct = 0;
    s_job.text[0] = s_job.error[0] = 0;
    xSemaphoreGive(s_job_mtx);
}

static void job_set(int slot, int pct, const char *text)
{
    xSemaphoreTake(s_job_mtx, portMAX_DELAY);
    s_job.slot = slot;
    s_job.pct = pct;
    if (text) strlcpy(s_job.text, text, sizeof(s_job.text));
    xSemaphoreGive(s_job_mtx);
}

static void job_end(const char *err)
{
    xSemaphoreTake(s_job_mtx, portMAX_DELAY);
    s_job.active = false;
    strlcpy(s_job.error, err ? err : "", sizeof(s_job.error));
    s_job.seq++;
    xSemaphoreGive(s_job_mtx);
}

/* ---------- menu ---------- */

static void refresh_snapshot(void)
{
    basecamp_slot_t all[APPS_MAX];
    uint8_t ord[APPS_MAX];
    slots_get_all(all);
    meta_get_order(ord);
    ui_set_apps(all, ord);
}

static void show_menu(void)
{
    refresh_snapshot();
    ui_show_menu();
    ui_set_footer(s_footer);
}

void ctl_menu_changed(void)
{
    if (ui_menu_visible()) {
        show_menu();
    } else {
        refresh_snapshot();
    }
}

void ctl_upload_progress(int slot, int pct)
{
    if (s_up_slot != slot) {
        const char *names[APPS_MAX] = {"Uploading", "Uploading", "Uploading"};
        uint32_t colors[APPS_MAX] = {meta_get_color(0), meta_get_color(1), meta_get_color(2)};
        ui_show_progress("RECEIVING", 1u << slot, names, colors);
        ui_status("Receiving the app from your phone ...");
        s_up_slot = slot;
    }
    ui_progress_slot(slot, pct, "receiving", false);
}

void ctl_upload_finished(void)
{
    s_up_slot = -1;
    show_menu();
}

/* ---------- Wi-Fi mode ---------- */

/* The line under the title: the setup network, or the home network and this device's address. */
static void set_hint(const char *text)
{
    ui_set_hint(text);                         /* in place: no menu rebuild */
}

static void hint_home(void)
{
    char ip[16], hint[64];
    net_sta_ip(ip);
    snprintf(hint, sizeof(hint), "%.24s  -  %s", net_ssid(), ip[0] ? ip : "no address");
    set_hint(hint);
}

/* Decide how the device is reachable:
 *  - no saved Wi-Fi: the open setup network "BaseCamp-Setup";
 *  - saved Wi-Fi that can be joined: join it and keep the setup network OFF
 *    (the web page is then on the home network, address shown on screen);
 *  - saved Wi-Fi that cannot be joined: the setup network comes back so it can be fixed. */
static void apply_wifi_mode(void)
{
    if (!net_has_credentials()) {
        net_ap_start();
        set_hint(NET_AP_SSID "  -  " NET_AP_IP);
        return;
    }
    char hint[64];
    snprintf(hint, sizeof(hint), "Joining %.32s ...", net_ssid());
    set_hint(hint);
    if (net_sta_start() == ESP_OK) {
        for (int round = 0; round < 2; round++) {          /* a slow router gets a second try */
            net_sta_connect();
            if (!net_sta_wait(20000)) {
                continue;
            }
            if (net_ap_active()) {
                /* let the phone that just saved the network read the answer, and never cut an upload */
                for (int i = 0; i < 120 && ctl_busy(); i++) vTaskDelay(pdMS_TO_TICKS(500));
                vTaskDelay(pdMS_TO_TICKS(2500));
                net_ap_stop();
            }
            hint_home();
            return;
        }
    }
    /* Could not join: keep / bring back the setup network, and say why. */
    net_ap_start();
    snprintf(hint, sizeof(hint), NET_AP_SSID "  -  %.12s: %s", net_ssid(),
             net_fail_text()[0] ? net_fail_text() : "Wi-Fi not available");
    set_hint(hint);
}

static volatile bool s_wifi_busy, s_wifi_again;

static void wifi_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));           /* let the first frame and the supply settle before the radio starts */
    do {
        s_wifi_again = false;
        apply_wifi_mode();
    } while (s_wifi_again);
    s_wifi_busy = false;
    vTaskDelete(NULL);
}

static void wifi_kick(void)
{
    if (__atomic_exchange_n(&s_wifi_busy, true, __ATOMIC_ACQ_REL)) {
        s_wifi_again = true;                   /* already working: run once more afterwards */
        return;
    }
    if (xTaskCreate(wifi_task, "wifi", 6144, NULL, 3, NULL) != pdPASS) {
        s_wifi_busy = false;
    }
}

void ctl_wifi_changed(void)
{
    wifi_kick();
}

/* ---------- Wi-Fi gate ---------- */

/* Connect to the home Wi-Fi, asking for it on screen (and in the web page) when needed.
 * `restore` redraws the screen that was showing before. */
static bool ensure_station(void (*restore)(void *), void *ctx)
{
    for (;;) {
        if (net_has_credentials()) {
            char msg[112];
            snprintf(msg, sizeof(msg), "Connecting to %.32s ...", net_ssid());
            ui_status(msg);
            net_sta_connect();
            if (net_sta_wait(25000)) {
                if (net_ap_active()) {         /* a home Wi-Fi works: the setup network is no longer needed */
                    net_ap_stop();
                    hint_home();
                }
                return true;
            }
            snprintf(msg, sizeof(msg), "Could not connect to \"%.32s\" (%s). Check it in the web page.", net_ssid(),
                     net_fail_text()[0] ? net_fail_text() : "no answer");
            ui_show_wifi_setup(msg);
        } else {
            ui_show_wifi_setup(NULL);
        }
        net_wait_creds(UINT32_MAX);            /* "Cancel" restarts the launcher */
        if (restore) restore(ctx);
    }
}

/* ---------- installs ---------- */

static void progress_cb(int slot, int pct, void *ctx)
{
    ui_progress_slot(slot, pct, (const char *)ctx, false);
    job_set(slot, pct, (const char *)ctx);
}

typedef struct {
    const char *title;
    uint32_t mask;
    const char *names[APPS_MAX];
    uint32_t colors[APPS_MAX];
} progress_ctx_t;

static void restore_progress(void *c)
{
    progress_ctx_t *p = c;
    ui_show_progress(p->title, p->mask, p->names, p->colors);
}

static void store_meta(const inst_job_t *j, const char *tag)
{
    meta_clear_slot(j->slot);                  /* new image, new baseline; drops the old app's data */
    meta_set_name(j->slot, j->name);
    meta_set_color(j->slot, j->color);
    meta_set_src(j->slot, &j->src);
    if (tag) meta_set_tag(j->slot, tag);
}

static void do_batch(bool update)
{
    progress_ctx_t pc = {.title = update ? "UPDATING" : "INSTALLING", .names = {"", "", ""}};
    for (int i = 0; i < s_nbatch; i++) {
        int s = s_batch[i].slot;
        pc.mask |= 1u << s;
        pc.names[s] = s_batch[i].name;
        pc.colors[s] = s_batch[i].color;
    }
    job_begin();
    job_set(s_batch[0].slot, 0, "connecting to Wi-Fi");
    restore_progress(&pc);
    s_nfailed = 0;

    if (!ensure_station(restore_progress, &pc)) {
        job_end("Wi-Fi not connected");
        return;
    }
    restore_progress(&pc);
    ui_status(update ? "Updates keep each app's settings." : "Downloading from GitHub ...");

    char first_err[160] = "";
    for (int i = 0; i < s_nbatch; i++) {
        const inst_job_t *j = &s_batch[i];
        char err[96] = "";
        static inst_release_t rel;
        esp_err_t e = ESP_FAIL;
        bool erased = false;
        basecamp_slot_t old;
        slots_get(j->slot, &old);
        bool was_new = !slots_has_card(&old);

        /* GitHub hiccups (5xx, timeouts, dropped connections) are retried
         * automatically; wrong app / board / size errors are not. */
        for (int attempt = 1; attempt <= INSTALL_ATTEMPTS; attempt++) {
            char state[48];
            if (attempt > 1) {
                snprintf(state, sizeof(state), "retry %d/%d in %ds", attempt, INSTALL_ATTEMPTS, 3 * (attempt - 1));
                ui_progress_slot(j->slot, 0, state, false);
                job_set(j->slot, 0, state);
                vTaskDelay(pdMS_TO_TICKS(3000 * (attempt - 1)));
            }
            ui_progress_slot(j->slot, 0, "checking GitHub", false);
            job_set(j->slot, 0, "checking GitHub");
            e = inst_fetch_latest(&j->src, &rel, err, sizeof(err));
            if (e == ESP_OK) {
                snprintf(state, sizeof(state), "%.20s", rel.tag);
                ui_progress_slot(j->slot, 0, state, false);
                e = inst_install(j->slot, &j->src, &rel, progress_cb, state, &erased, err, sizeof(err));
            }
            if (e != ESP_ERR_TIMEOUT) {
                break;
            }
        }
        if (e == ESP_OK) {
            store_meta(j, rel.tag);
            if (was_new) meta_move_to_end(j->slot);
            ui_progress_slot(j->slot, 100, "installed", false);
        } else {
            if (erased) store_meta(j, NULL);   /* slot is blank now: keep a grey "retry" card */
            s_failed[s_nfailed++] = *j;
            ui_progress_slot(j->slot, -1, err, true);
            if (!first_err[0]) snprintf(first_err, sizeof(first_err), "%.31s: %.95s", j->name, err);
        }
        slots_scan_one(j->slot);
    }

    refresh_snapshot();
    if (!s_nfailed) {
        job_end(NULL);
        ui_status("All done");
        vTaskDelay(pdMS_TO_TICKS(1200));
        show_menu();
        return;
    }
    job_end(first_err);
    ui_status(first_err);
    ui_action_pair("Retry", UI_REQ_RETRY_BATCH, 0, "Menu", UI_REQ_MENU, 0);
}

/* ---------- updates check ---------- */

static const char *strip_v(const char *s)
{
    return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
}

typedef struct {
    int n;
    int slot[APPS_MAX];
    const char *names[APPS_MAX];
    uint32_t colors[APPS_MAX];
    basecamp_slot_t snap[APPS_MAX];
} upd_ctx_t;

static void restore_updates(void *c)
{
    upd_ctx_t *u = c;
    ui_show_updates(u->n, u->names, u->colors);
}

static void do_check_updates(void)
{
    static upd_ctx_t u;
    memset(&u, 0, sizeof u);
    slots_get_all(u.snap);
    uint8_t ord[APPS_MAX], list[APPS_MAX];
    meta_get_order(ord);
    unsigned mask = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        inst_source_t src;
        if (u.snap[i].state == SLOT_APP && meta_get_src(i, &src)) mask |= 1u << i;
    }
    u.n = apps_build_order(ord, mask, list);
    for (int k = 0; k < u.n; k++) {
        u.slot[k] = list[k];
        u.names[k] = u.snap[list[k]].name;
        u.colors[k] = u.snap[list[k]].color;
    }
    ui_show_updates(u.n, u.names, u.colors);
    if (u.n == 0) {
        ui_status("No installed app has a GitHub source, so there is nothing to update here.");
        ui_action_button("Back", UI_REQ_MENU, 0);
        return;
    }
    if (!ensure_station(restore_updates, &u)) {
        return;
    }
    restore_updates(&u);
    ui_status("Checking GitHub releases ...");

    unsigned avail = 0;
    for (int k = 0; k < u.n; k++) {
        const basecamp_slot_t *s = &u.snap[u.slot[k]];
        static inst_release_t rel;
        inst_source_t src;
        char err[96], row[140];
        meta_get_src(u.slot[k], &src);
        esp_err_t fe = ESP_ERR_TIMEOUT;
        for (int attempt = 0; attempt < INSTALL_ATTEMPTS && fe == ESP_ERR_TIMEOUT; attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
            fe = inst_fetch_latest(&src, &rel, err, sizeof(err));
        }
        if (fe != ESP_OK) {
            snprintf(row, sizeof(row), "check failed: %.95s", err);
            ui_updates_row(k, row, false);
            continue;
        }
        const char *cur = s->tag[0] ? s->tag : (s->generic_desc ? "" : s->desc.version);
        bool same = cur[0] && strcmp(strip_v(cur), strip_v(rel.tag)) == 0;
        if (same) {
            snprintf(row, sizeof(row), "%.40s  (up to date)", cur);
        } else {
            snprintf(row, sizeof(row), "%.40s -> %.40s", cur[0] ? cur : "?", rel.tag);
            avail |= 1u << u.slot[k];
        }
        ui_updates_row(k, row, !same);
    }
    if (avail) {
        ui_status("Install updates? App settings are kept.");
        ui_action_pair("Install", UI_REQ_INSTALL_UPDATES, (int)avail, "Back", UI_REQ_MENU, 0);
    } else {
        ui_status("Everything is up to date.");
        ui_action_button("Back", UI_REQ_MENU, 0);
    }
}

/* ---------- launching ---------- */

static void launch(int slot)
{
    basecamp_slot_t s;
    slots_get(slot, &s);
    ESP_LOGI(TAG, "launching ota_%d (%s %s)", slot, s.name, s.version);
    ui_show_launching(s.name, s.color);
    vTaskDelay(pdMS_TO_TICKS(250));
    webui_stop();
    net_shutdown();
    board_display_off();
    basecamp_bootsel_set((uint8_t)slot);
    esp_restart();
}

/* ---------- work queue ---------- */

static void restart_launcher(void)
{
    webui_stop();
    net_shutdown();
    board_display_off();
    basecamp_bootsel_clear();
    esp_restart();
}

static void post(const work_t *w)
{
    if (xQueueSend(s_work, w, 0) != pdTRUE) {
        ctl_end();
    }
}

/* UI requests arrive on the LVGL task: hand them to the worker. */
static void on_request(ui_req_t req, int arg)
{
    if (req == UI_REQ_RESTART) {
        restart_launcher();                    /* also cancels anything in progress */
    }
    if (!ctl_begin()) {
        return;
    }
    work_t w = {.kind = W_REQ, .req = req, .arg = arg};
    post(&w);
}

esp_err_t ctl_start_install(int slot, const inst_source_t *src, const char *name, uint32_t color)
{
    if (!ctl_begin()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_nbatch = 1;
    s_batch[0].slot = slot;
    s_batch[0].src = *src;
    strlcpy(s_batch[0].name, name, sizeof(s_batch[0].name));
    s_batch[0].color = color;
    work_t w = {.kind = W_BATCH};
    post(&w);
    return ESP_OK;
}

static void batch_add(int slot, const inst_source_t *src, const char *name, uint32_t color)
{
    inst_job_t *j = &s_batch[s_nbatch++];
    j->slot = slot;
    j->src = *src;
    strlcpy(j->name, name, sizeof(j->name));
    j->color = color;
}

static void handle_req(const work_t *w)
{
    s_nbatch = 0;
    switch (w->req) {
    case UI_REQ_LAUNCH:
        launch(w->arg);
        break;
    case UI_REQ_RETRY_SLOT: {
        inst_source_t src;
        basecamp_slot_t s;
        slots_get(w->arg, &s);
        if (meta_get_src(w->arg, &src)) {
            batch_add(w->arg, &src, s.name, s.color);
            do_batch(false);
        }
        break;
    }
    case UI_REQ_INSTALL_DEFAULTS: {
        basecamp_slot_t all[APPS_MAX];
        slots_get_all(all);
        int k = 0;
        for (int i = 0; i < APPS_MAX && k < INST_PRESETS; i++) {
            if (all[i].state != SLOT_EMPTY) continue;
            inst_source_t src;
            char name[32];
            inst_preset(k++, &src, name);
            batch_add(i, &src, name, meta_get_color(i));
        }
        if (s_nbatch) do_batch(false);
        break;
    }
    case UI_REQ_INSTALL_UPDATES:
        for (int i = 0; i < APPS_MAX; i++) {
            inst_source_t src;
            basecamp_slot_t s;
            if (!((w->arg >> i) & 1) || !meta_get_src(i, &src)) continue;
            slots_get(i, &s);
            batch_add(i, &src, s.name, s.color);
        }
        if (s_nbatch) do_batch(true);
        break;
    case UI_REQ_RETRY_BATCH:
        for (int i = 0; i < s_nfailed; i++) s_batch[s_nbatch++] = s_failed[i];
        if (s_nbatch) do_batch(false);
        break;
    case UI_REQ_CHECK_UPDATES:
        do_check_updates();
        break;
    case UI_REQ_ACCEPT:
        slots_accept(w->arg);
        show_menu();
        break;
    case UI_REQ_MENU:
        show_menu();
        break;
    default:
        break;
    }
}

static void worker(void *arg)
{
    work_t w;
    for (;;) {
        xQueueReceive(s_work, &w, portMAX_DELAY);
        if (w.kind == W_BATCH) {
            do_batch(false);
        } else {
            handle_req(&w);
        }
        ctl_end();      /* result screens end with Retry / Menu / Back buttons */
    }
}

static void footer_task(void *arg)
{
    for (;;) {
        int pct = axp2101_battery_percent();
        bool usb = axp2101_vbus_present();
        if (pct >= 0) {
            snprintf(s_footer, sizeof(s_footer), "%s%d%%", usb ? "USB  " : "", pct);
        } else {
            snprintf(s_footer, sizeof(s_footer), "%s", usb ? "USB" : "");
        }
        if (s_abnormal_reset) {                /* the last start was caused by a fault, not a power-on */
            char tmp[112];
            snprintf(tmp, sizeof(tmp), "RESTARTED: %.14s  %s", s_boot_text, s_footer);
            strlcpy(s_footer, tmp, sizeof(s_footer));
        }
        ui_set_footer(s_footer);
        /* On the home network with no setup network up: if the link was lost for good (router
         * restarted, moved house), retry once and fall back to the setup network. */
        if (net_has_credentials() && !net_ap_active() && net_sta_state() == NET_STA_FAILED && !ctl_busy()) {
            wifi_kick();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static SemaphoreHandle_t s_radio_ready;
static volatile bool s_radio_ok;

/* All Wi-Fi bring-up (driver init, RF calibration, radio start) runs in its own task: if any of it
 * ever blocks, the menu still comes up. */
static void radio_boot_task(void *arg)
{
    if (net_init() == ESP_OK) {
        if (net_has_credentials()) {
            net_sta_start();
        } else {
            net_ap_start();
        }
        s_radio_ok = true;
    }
    xSemaphoreGive(s_radio_ready);
    vTaskDelete(NULL);
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
        ESP_LOGW(TAG, "nvs_flash_init: %s - app names, order and settings unavailable", esp_err_to_name(err));
    }

    record_boot();
    s_job_mtx = xSemaphoreCreateMutex();
    s_job.slot = -1;
    slots_init();

    /* Radio first, while the panel is still off: its start-up current surge then cannot show on the
     * screen. Connecting / mode hand-over follows once the menu is up (wifi_kick below). */
    s_radio_ready = xSemaphoreCreateBinary();
#if CONFIG_BASECAMP_ENABLE_WIFI
    bool net_ok = xTaskCreate(radio_boot_task, "radio", 4096, NULL, 1, NULL) == pdPASS;
#else
    bool net_ok = false;
#endif

    slots_scan_all();
    board_set_dark_start(true);                /* no white flash from the panel's random RAM at power-up */
    ESP_ERROR_CHECK(board_init(NULL));

    s_work = xQueueCreate(4, sizeof(work_t));
    ui_init(on_request);
    ui_set_hint("Starting Wi-Fi ...");
    show_menu();
    /* Light the panel only once the first frame has reached it and the radio has started (a Wi-Fi
     * problem must never keep the menu from appearing, so the wait is bounded). */
    bool radio_up = net_ok && xSemaphoreTake(s_radio_ready, pdMS_TO_TICKS(3000)) == pdTRUE && s_radio_ok;
    vTaskDelay(pdMS_TO_TICKS(200));
    board_set_brightness(100);
    xTaskCreate(worker, "worker", 10240, NULL, 5, NULL);
    xTaskCreate(footer_task, "footer", 3072, NULL, 2, NULL);

    /* Web page + Wi-Fi (setup network or the saved home network). The menu works even if this fails. */
    if (net_ok && radio_up && webui_start() == ESP_OK) {
        wifi_kick();
    } else {
        ui_set_hint("Wi-Fi unavailable");
        show_menu();
        ESP_LOGW(TAG, "Wi-Fi / web UI not available");
    }
}
