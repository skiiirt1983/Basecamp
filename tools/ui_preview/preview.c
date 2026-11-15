/*
 * PC preview of the launcher screens: compiles LVGL + the real main/ui.c and
 * renders each screen to a 466x466 RGB565 dump (tools/ui_preview/run.sh turns
 * them into PNGs with the round display mask applied).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "ui.h"

#define W 466
static uint16_t fb[W * W];
static uint16_t draw_buf[W * W];
static uint32_t tick_ms;

static uint32_t get_tick(void) { return tick_ms; }
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(&fb[y * W + a->x1], src + (y - a->y1) * w, w * 2);
    }
    lv_display_flush_ready(d);
}

/* slots_has_card() lives in slots.c (ESP-only); same rule here. */
bool slots_has_card(const basecamp_slot_t *s)
{
    return s->state == SLOT_APP || s->state == SLOT_DAMAGED || (s->state == SLOT_EMPTY && s->has_src);
}

static void render(const char *name)
{
    for (int i = 0; i < 40; i++) {
        tick_ms += 30;
        lv_timer_handler();
    }
    char path[128];
    snprintf(path, sizeof path, "%s/%s.rgb565", getenv("OUT_DIR") ? getenv("OUT_DIR") : ".", name);
    FILE *f = fopen(path, "wb");
    fwrite(fb, 2, W * W, f);
    fclose(f);
    printf("rendered %s\n", name);
}

static void req_cb(ui_req_t r, int a) { printf("  request %d %d\n", r, a); }

static basecamp_slot_t app(int slot, const char *name, const char *ver, uint32_t col)
{
    basecamp_slot_t s = {0};
    s.state = SLOT_APP; s.color = col; s.has_src = true;
    strcpy(s.name, name); strcpy(s.version, ver);
    snprintf(s.detail, sizeof s.detail, "proj");
    static esp_partition_t parts[3];
    snprintf(parts[slot].label, sizeof parts[slot].label, "ota_%d", slot);
    s.part = &parts[slot];
    return s;
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(get_tick);
    lv_display_t *d = lv_display_create(W, W);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);
    ui_init(req_cb);
    ui_set_hint("BaseCamp-Setup  -  192.168.4.1");

    basecamp_slot_t s[3];
    uint8_t order[3] = {0, 1, 2};
    s[0] = app(0, "PrintSphere", "v1.6.2", 0xFF7A1A);
    s[1] = app(1, "Capsule Radar", "v1.6.0", 0x18C8B0);
    s[2] = app(2, "LyricFlash", "1.0.2", 0xC850F0);
    ui_set_apps(s, order); ui_show_menu(); ui_set_footer("USB  85%"); render("menu3");

    strcpy(s[1].name, "My Very Long Custom App Name"); s[1].changed = true; s[2].has_src = false;
    ui_set_apps(s, order); ui_show_menu(); ui_set_footer("72%"); render("menu3_long_changed");

    uint8_t rev[3] = {2, 0, 1};
    ui_set_apps(s, rev); ui_show_menu(); render("menu3_reordered");

    s[2].state = SLOT_EMPTY; memset(s[2].name, 0, sizeof s[2].name);
    s[2].has_src = false;
    ui_set_apps(s, order); ui_show_menu(); render("menu2");

    s[1].state = SLOT_EMPTY;
    ui_set_apps(s, order); ui_show_menu(); render("menu1");

    s[0].state = SLOT_DAMAGED; strcpy(s[0].detail, "wrong board");
    s[1] = app(1, "Capsule Radar", "v1.6.0", 0x18C8B0); s[1].state = SLOT_EMPTY; strcpy(s[1].detail, "install failed");
    ui_set_apps(s, order); ui_show_menu(); render("menu_grey_retry");

    memset(s, 0, sizeof s);
    ui_set_apps(s, order); ui_show_menu(); ui_set_footer("USB"); render("menu0_empty");

    const char *names[3] = {"PrintSphere", "Capsule Radar", "LyricFlash"};
    uint32_t cols[3] = {0xFF7A1A, 0x18C8B0, 0xC850F0};
    ui_show_progress("INSTALLING", 7, names, cols);
    ui_progress_slot(0, 100, "installed", false);
    ui_progress_slot(1, 42, "downloading v1.6.0", false);
    ui_progress_slot(2, 7, "retry 2/4 in 3s", false);
    ui_status("Downloading from GitHub ..."); render("progress3");

    ui_progress_slot(2, -1, "built for a different board", true);
    ui_status("LyricFlash: built for a different board");
    ui_action_pair("Retry", UI_REQ_RETRY_BATCH, 0, "Menu", UI_REQ_MENU, 0); render("progress_failed");

    const char *one[3] = {"My Uploaded", "", ""};
    ui_show_progress("RECEIVING", 1, one, cols); ui_progress_slot(0, 63, "receiving", false);
    ui_status("Receiving the app from your phone ..."); render("progress1");

    ui_show_wifi_setup(NULL); render("wifi_needed");
    ui_show_wifi_setup("Could not connect to \"HomeNet\". Check it in the web page."); render("wifi_failed");

    const char *un[2] = {"PrintSphere", "LyricFlash"}; uint32_t uc[2] = {0xFF7A1A, 0xC850F0};
    ui_show_updates(2, un, uc);
    ui_updates_row(0, "v1.6.1 -> v1.6.2", true); ui_updates_row(1, "1.0.2  (up to date)", false);
    ui_status("Install updates? App settings are kept.");
    ui_action_pair("Install", UI_REQ_INSTALL_UPDATES, 1, "Back", UI_REQ_MENU, 0); render("updates");

    ui_show_launching("Capsule Radar", 0x18C8B0); render("launching");
    return 0;
}
