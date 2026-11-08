/*
 * Round launcher UI for the 466x466 AMOLED.
 *   menu      : three stacked pill cards inside the circle, title, status line
 *               and a small "Updates" button.
 *   wifi      : how to join the Basecamp-Setup portal.
 *   progress  : one ring per app being installed + status text.
 *   updates   : current -> available per app, install button.
 * Everything that touches the network runs in main.c's worker task, which
 * calls these functions; they take the (recursive) LVGL port lock.
 */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#define BTN_W     300
#define BTN_H     92
#define BTN_PITCH 104
#define CARD_Y0   8      /* vertical offset of the middle card */

static const uint32_t s_accent[BASECAMP_NUM_SLOTS] = {0xFF7A1A, 0x18C8B0, 0xC850F0};

static ui_request_cb_t s_cb;
static const basecamp_slot_t *s_slots;

static lv_obj_t *s_card[BASECAMP_NUM_SLOTS];
static lv_obj_t *s_card_name[BASECAMP_NUM_SLOTS];
static lv_obj_t *s_card_sub[BASECAMP_NUM_SLOTS];
static lv_obj_t *s_footer;
static lv_obj_t *s_busy;

static lv_obj_t *s_ring[BASECAMP_NUM_SLOTS];
static lv_obj_t *s_ring_pct[BASECAMP_NUM_SLOTS];
static lv_obj_t *s_ring_state[BASECAMP_NUM_SLOTS];
static lv_obj_t *s_status;
static lv_obj_t *s_action;
static lv_obj_t *s_rows[BASECAMP_NUM_SLOTS];

#define LOCK()   lvgl_port_lock(0)
#define UNLOCK() lvgl_port_unlock()

static void request(ui_req_t r, int arg)
{
    if (s_cb) {
        s_cb(r, arg);
    }
}

/* ---------- common helpers ---------- */

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* Forget per-screen widgets; they die with the old screen. */
    memset(s_card, 0, sizeof(s_card));
    memset(s_ring, 0, sizeof(s_ring));
    memset(s_rows, 0, sizeof(s_rows));
    s_footer = s_status = s_action = s_busy = NULL;
    return scr;
}

static void load(lv_obj_t *scr)
{
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *title(lv_obj_t *scr, const char *txt)
{
    lv_obj_t *t = label(scr, &lv_font_montserrat_20, 0x9A9A9A, txt);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 42);
    return t;
}

static void pill_style(lv_obj_t *b, uint32_t accent)
{
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_hex(accent), lv_color_black(), 70), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
}

static void action_cb(lv_event_t *e)
{
    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
    request((ui_req_t)(packed >> 16), (int)(packed & 0xFFFF));
}

static lv_obj_t *small_button(lv_obj_t *p, const char *txt, int y, ui_req_t req, int arg)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, 150, 44);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
    pill_style(b, 0x8A8A8A);
    lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(((uint32_t)req << 16) | (arg & 0xFFFF)));
    lv_obj_t *l = label(b, &lv_font_montserrat_20, 0xFFFFFF, txt);
    lv_obj_center(l);
    return b;
}

/* ---------- menu ---------- */

static void card_text(int i)
{
    const basecamp_slot_t *s = &s_slots[i];
    if (!s_card[i]) {
        return;
    }
    lv_label_set_text(s_card_name[i], s->name);
    const char *label_txt = s->part ? s->part->label : "?";
    if (!s->valid) {
        lv_obj_add_state(s_card[i], LV_STATE_DISABLED);
        lv_label_set_text_fmt(s_card_sub[i], LV_SYMBOL_DOWNLOAD " %s - tap to install", s->detail);
        lv_obj_set_style_text_color(s_card_name[i], lv_color_hex(0x5A5A5A), 0);
        lv_obj_set_style_text_color(s_card_sub[i], lv_color_hex(0x6A6A6A), 0);
    } else {
        lv_obj_remove_state(s_card[i], LV_STATE_DISABLED);
        lv_obj_set_style_text_color(s_card_name[i], lv_color_white(), 0);
        if (s->changed) {
            lv_label_set_text_fmt(s_card_sub[i], LV_SYMBOL_WARNING " %s  changed", s->version);
            lv_obj_set_style_text_color(s_card_sub[i], lv_color_hex(0xFFB020), 0);
        } else {
            lv_label_set_text_fmt(s_card_sub[i], "%s  -  %s", s->version, label_txt);
            lv_obj_set_style_text_color(s_card_sub[i], lv_color_hex(0xC8C8C8), 0);
        }
    }
}

static void card_click_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_busy) {
        return;
    }
    if (s_slots[slot].valid) {
        request(UI_REQ_LAUNCH, slot);
    } else {
        request(UI_REQ_INSTALL, 1 << slot);
    }
}

static void card_long_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_slots[slot].changed) {
        request(UI_REQ_ACCEPT, slot);
    }
}

static void make_card(lv_obj_t *scr, int i)
{
    lv_obj_t *btn = lv_button_create(scr);
    s_card[i] = btn;
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, CARD_Y0 + (i - 1) * BTN_PITCH);
    pill_style(btn, s_accent[i]);
    lv_obj_set_style_border_width(btn, 3, 0);
    /* Empty / failed slots look grey but stay tappable (retry install). */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x3A3A3A), LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(btn, 6, 0);
    lv_obj_add_event_cb(btn, card_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(btn, card_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    s_card_name[i] = label(btn, &lv_font_montserrat_28, 0xFFFFFF, "");
    lv_label_set_long_mode(s_card_name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_card_name[i], BTN_W - 40);
    lv_obj_align(s_card_name[i], LV_ALIGN_CENTER, 0, -14);

    s_card_sub[i] = label(btn, &lv_font_montserrat_20, 0xC8C8C8, "");
    lv_label_set_long_mode(s_card_sub[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_card_sub[i], BTN_W - 50);
    lv_obj_align(s_card_sub[i], LV_ALIGN_CENTER, 0, 22);
    card_text(i);
}

void ui_init(const basecamp_slot_t slots[BASECAMP_NUM_SLOTS], ui_request_cb_t cb)
{
    s_slots = slots;
    s_cb = cb;
}

void ui_show_menu(void)
{
    LOCK();
    lv_obj_t *scr = new_screen();
    title(scr, "BASECAMP");
    s_footer = label(scr, &lv_font_montserrat_14, 0x7A7A7A, "");
    lv_obj_align(s_footer, LV_ALIGN_CENTER, 0, -158);
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        make_card(scr, i);
    }
    lv_obj_t *upd = small_button(scr, LV_SYMBOL_REFRESH " Updates", 186, UI_REQ_CHECK_UPDATES, 0);
    lv_obj_set_size(upd, 150, 38);
    lv_obj_set_style_text_font(lv_obj_get_child(upd, 0), &lv_font_montserrat_14, 0);
    load(scr);
    UNLOCK();
}

void ui_menu_refresh_slot(int slot)
{
    LOCK();
    card_text(slot);
    UNLOCK();
}

void ui_set_footer(const char *text)
{
    LOCK();
    if (s_footer) {
        lv_label_set_text(s_footer, text);
    }
    UNLOCK();
}

void ui_show_launching(int slot)
{
    LOCK();
    lv_obj_t *scr = new_screen();
    s_busy = scr;
    lv_obj_t *ring = lv_arc_create(scr);
    lv_obj_set_size(ring, 420, 420);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 100);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(ring, lv_color_hex(s_accent[slot]), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *l = label(scr, &lv_font_montserrat_28, 0xFFFFFF, "");
    lv_label_set_text_fmt(l, "Starting\n%s", s_slots[slot].name);
    lv_obj_center(l);
    load(scr);
    UNLOCK();
}

/* ---------- Wi-Fi setup ---------- */

void ui_show_wifi_setup(const char *notice)
{
    LOCK();
    lv_obj_t *scr = new_screen();
    title(scr, "WI-FI SETUP");
    lv_obj_t *steps = label(scr, &lv_font_montserrat_20, 0xFFFFFF, "On your phone, join the Wi-Fi");
    lv_obj_align(steps, LV_ALIGN_CENTER, 0, -110);
    lv_obj_t *ssid = label(scr, &lv_font_montserrat_28, 0xFF7A1A, "Basecamp-Setup");
    lv_obj_align(ssid, LV_ALIGN_CENTER, 0, -70);
    lv_obj_t *more = label(scr, &lv_font_montserrat_20, 0xC8C8C8,
                           "The setup page opens by itself.\n"
                           "If not, browse to 192.168.4.1\n"
                           "Pick your network, enter\nthe password, tap Connect.");
    lv_obj_align(more, LV_ALIGN_CENTER, 0, 20);
    s_status = label(scr, &lv_font_montserrat_14, 0xFF6A6A, notice ? notice : "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 300);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 105);
    small_button(scr, "Cancel", 160, UI_REQ_RESTART, 0);
    load(scr);
    UNLOCK();
}

/* ---------- install progress ---------- */

void ui_show_progress(const char *ttl, uint32_t mask)
{
    LOCK();
    lv_obj_t *scr = new_screen();
    s_busy = scr;
    title(scr, ttl);
    int n = 0;
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        n += (mask >> i) & 1;
    }
    int k = 0;
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        if (!((mask >> i) & 1)) {
            continue;
        }
        int x = (n == 1) ? 0 : (n == 2) ? (k ? 75 : -75) : (k - 1) * 135;
        k++;
        lv_obj_t *ring = lv_arc_create(scr);
        s_ring[i] = ring;
        lv_obj_set_size(ring, 116, 116);
        lv_obj_align(ring, LV_ALIGN_CENTER, x, -30);
        lv_arc_set_bg_angles(ring, 0, 360);
        lv_arc_set_rotation(ring, 270);
        lv_arc_set_range(ring, 0, 100);
        lv_arc_set_value(ring, 0);
        lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(ring, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ring, lv_color_hex(0x262626), LV_PART_MAIN);
        lv_obj_set_style_arc_color(ring, lv_color_hex(s_accent[i]), LV_PART_INDICATOR);

        s_ring_pct[i] = label(scr, &lv_font_montserrat_20, 0xFFFFFF, "--");
        lv_obj_align_to(s_ring_pct[i], ring, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *nm = label(scr, &lv_font_montserrat_14, 0xFFFFFF, s_slots[i].name);
        lv_obj_align_to(nm, ring, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        s_ring_state[i] = label(scr, &lv_font_montserrat_14, 0x8A8A8A, "waiting");
        lv_obj_set_width(s_ring_state[i], 130);
        lv_label_set_long_mode(s_ring_state[i], LV_LABEL_LONG_DOT);
        lv_obj_align_to(s_ring_state[i], nm, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    s_status = label(scr, &lv_font_montserrat_14, 0xC8C8C8, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 320);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 105);
    load(scr);
    UNLOCK();
}

void ui_progress_slot(int slot, int percent, const char *state, bool failed)
{
    LOCK();
    if (s_ring[slot]) {
        if (percent >= 0) {
            lv_arc_set_value(s_ring[slot], percent);
            lv_label_set_text_fmt(s_ring_pct[slot], "%d%%", percent);
        }
        if (failed) {
            lv_obj_set_style_arc_color(s_ring[slot], lv_color_hex(0xFF4040), LV_PART_INDICATOR);
            lv_arc_set_value(s_ring[slot], 100);
            lv_label_set_text(s_ring_pct[slot], LV_SYMBOL_CLOSE);
        } else if (percent >= 100 && state && strcmp(state, "installed") == 0) {
            lv_label_set_text(s_ring_pct[slot], LV_SYMBOL_OK);
        }
        if (state) {
            lv_label_set_text(s_ring_state[slot], state);
            lv_obj_set_style_text_color(s_ring_state[slot],
                                        lv_color_hex(failed ? 0xFF6A6A : 0x8A8A8A), 0);
        }
    }
    UNLOCK();
}

void ui_status(const char *text)
{
    LOCK();
    if (s_status) {
        lv_label_set_text(s_status, text);
    }
    UNLOCK();
}

void ui_action_button(const char *txt, ui_req_t req, int arg)
{
    LOCK();
    if (s_action) {
        lv_obj_delete(s_action);
    }
    s_action = small_button(lv_screen_active(), txt, 160, req, arg);
    UNLOCK();
}

void ui_action_pair(const char *left, ui_req_t lreq, int larg,
                    const char *right, ui_req_t rreq, int rarg)
{
    LOCK();
    if (s_action) {
        lv_obj_delete(s_action);
    }
    /* Container so a later ui_action_button() replaces both. */
    s_action = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_action);
    lv_obj_set_size(s_action, 300, 50);
    lv_obj_align(s_action, LV_ALIGN_CENTER, 0, 152);
    lv_obj_t *a = small_button(s_action, left, 0, lreq, larg);
    lv_obj_set_size(a, 140, 44);
    lv_obj_align(a, LV_ALIGN_CENTER, -76, 0);
    lv_obj_t *b = small_button(s_action, right, 0, rreq, rarg);
    lv_obj_set_size(b, 140, 44);
    lv_obj_align(b, LV_ALIGN_CENTER, 76, 0);
    pill_style(a, 0x18C8B0);
    UNLOCK();
}

/* ---------- updates ---------- */

void ui_show_updates(void)
{
    LOCK();
    lv_obj_t *scr = new_screen();
    s_busy = scr;
    title(scr, "UPDATES");
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        lv_obj_t *nm = label(scr, &lv_font_montserrat_20, 0xFFFFFF, s_slots[i].name);
        lv_obj_set_style_text_color(nm, lv_color_hex(s_accent[i]), 0);
        lv_obj_align(nm, LV_ALIGN_CENTER, 0, -118 + i * 70);
        s_rows[i] = label(scr, &lv_font_montserrat_20, 0xC8C8C8, "checking...");
        lv_obj_set_width(s_rows[i], 320);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_rows[i], LV_ALIGN_CENTER, 0, -90 + i * 70);
    }
    s_status = label(scr, &lv_font_montserrat_14, 0xC8C8C8, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 320);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 105);
    load(scr);
    UNLOCK();
}

void ui_updates_row(int slot, const char *text, bool available)
{
    LOCK();
    if (s_rows[slot]) {
        lv_label_set_text(s_rows[slot], text);
        lv_obj_set_style_text_color(s_rows[slot], lv_color_hex(available ? 0x7CFF7C : 0xC8C8C8), 0);
    }
    UNLOCK();
}
