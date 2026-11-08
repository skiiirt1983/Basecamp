/*
 * Round launcher UI for the 466x466 AMOLED: three stacked pill buttons that
 * stay inside the circle, a title on top and a status footer at the bottom.
 */
#include <stdio.h>
#include "ui.h"
#include "lvgl.h"

#define BTN_W     300
#define BTN_H     96
#define BTN_PITCH 110

static const uint32_t s_accent[BASECAMP_NUM_SLOTS] = {0xFF7A1A, 0x18C8B0, 0xC850F0};

static ui_launch_cb_t s_cb;
static ui_accept_cb_t s_accept_cb;
static lv_obj_t *s_sub[BASECAMP_NUM_SLOTS];
static const basecamp_slot_t *s_slots;
static lv_obj_t *s_footer;
static lv_obj_t *s_overlay;

static void launch_timer_cb(lv_timer_t *t)
{
    int slot = (int)(intptr_t)lv_timer_get_user_data(t);
    lv_timer_delete(t);
    if (s_cb) {
        s_cb(slot);
    }
}

static void set_sub_text(int i)
{
    const basecamp_slot_t *s = &s_slots[i];
    const char *label = s->part ? s->part->label : "?";
    if (!s->valid) {
        lv_label_set_text_fmt(s_sub[i], "%s  -  %s", s->detail, label);
        lv_obj_set_style_text_color(s_sub[i], lv_color_hex(0x4A4A4A), 0);
    } else if (s->changed) {
        lv_label_set_text_fmt(s_sub[i], LV_SYMBOL_WARNING " %s  changed", s->version);
        lv_obj_set_style_text_color(s_sub[i], lv_color_hex(0xFFB020), 0);
    } else {
        lv_label_set_text_fmt(s_sub[i], "%s  -  %s", s->version, label);
        lv_obj_set_style_text_color(s_sub[i], lv_color_hex(0xC8C8C8), 0);
    }
}

static void long_press_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_slots[slot].changed && s_accept_cb) {
        s_accept_cb(slot);
        set_sub_text(slot);
    }
}

static void btn_event_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_overlay || !s_slots[slot].valid) {
        return;
    }
    /* Full-screen "starting" overlay; give LVGL a few frames to show it. */
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);

    lv_obj_t *ring = lv_arc_create(s_overlay);
    lv_obj_set_size(ring, 420, 420);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 100);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(ring, lv_color_hex(s_accent[slot]), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(s_overlay);
    lv_label_set_text_fmt(l, "Starting\n%s", s_slots[slot].name);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);

    lv_timer_create(launch_timer_cb, 250, (void *)(intptr_t)slot);
}

static lv_obj_t *make_button(lv_obj_t *parent, int i)
{
    const basecamp_slot_t *s = &s_slots[i];
    lv_color_t accent = lv_color_hex(s_accent[i]);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (i - 1) * BTN_PITCH);
    lv_obj_set_style_radius(btn, BTN_H / 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_mix(accent, lv_color_black(), 70), 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_bg_color(btn, accent, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x3A3A3A), LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(btn, 6, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(btn, long_press_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    lv_obj_t *name = lv_label_create(btn);
    lv_label_set_text(name, s->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, BTN_W - 40);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *sub = lv_label_create(btn);
    s_sub[i] = sub;
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, BTN_W - 60);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 22);
    set_sub_text(i);

    if (s->valid) {
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(name, lv_color_hex(0x5A5A5A), 0);
    }
    return btn;
}

void ui_create(const basecamp_slot_t slots[BASECAMP_NUM_SLOTS], ui_launch_cb_t launch_cb,
               ui_accept_cb_t accept_cb)
{
    s_slots = slots;
    s_cb = launch_cb;
    s_accept_cb = accept_cb;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BASECAMP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9A9A9A), 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);

    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        make_button(scr, i);
    }

    s_footer = lv_label_create(scr);
    lv_label_set_text(s_footer, "");
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x7A7A7A), 0);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -44);
}

void ui_set_footer(const char *text)
{
    if (s_footer) {
        lv_label_set_text(s_footer, text);
    }
}
