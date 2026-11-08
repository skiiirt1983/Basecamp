#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "slots.h"

/* Requests from the UI (raised on the LVGL task, handled by main.c). */
typedef enum {
    UI_REQ_LAUNCH,          /* arg = slot */
    UI_REQ_INSTALL,         /* arg = slot mask: (re)install from GitHub */
    UI_REQ_ACCEPT,          /* arg = slot: accept changed image as baseline */
    UI_REQ_CHECK_UPDATES,
    UI_REQ_INSTALL_UPDATES, /* arg = slot mask */
    UI_REQ_RESTART,         /* back to a fresh menu */
} ui_req_t;

typedef void (*ui_request_cb_t)(ui_req_t req, int arg);

/* All functions take the LVGL port lock themselves (it is recursive). */
void ui_init(const basecamp_slot_t slots[BASECAMP_NUM_SLOTS], ui_request_cb_t cb);

void ui_show_menu(void);
void ui_menu_refresh_slot(int slot);
void ui_show_launching(int slot);
void ui_set_footer(const char *text);

/* Wi-Fi setup instructions (SoftAP + captive portal). */
void ui_show_wifi_setup(const char *notice);

/* Install progress: one ring per slot in mask. */
void ui_show_progress(const char *title, uint32_t mask);
void ui_progress_slot(int slot, int percent, const char *state, bool failed);
void ui_status(const char *text);
/* Show a single action button at the bottom of the current screen. */
void ui_action_button(const char *label, ui_req_t req, int arg);
/* Two buttons side by side (left is highlighted). */
void ui_action_pair(const char *left, ui_req_t lreq, int larg,
                    const char *right, ui_req_t rreq, int rarg);

/* Updates screen. */
void ui_show_updates(void);
void ui_updates_row(int slot, const char *text, bool available);
