#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "slots.h"

/* Requests from the UI (raised on the LVGL task, handled by main.c). */
typedef enum {
    UI_REQ_LAUNCH,           /* arg = slot */
    UI_REQ_RETRY_SLOT,       /* arg = slot: reinstall from its stored GitHub source */
    UI_REQ_INSTALL_DEFAULTS, /* empty menu: install the default apps */
    UI_REQ_ACCEPT,           /* arg = slot: accept a changed image as the new baseline */
    UI_REQ_CHECK_UPDATES,
    UI_REQ_INSTALL_UPDATES,  /* arg = slot mask */
    UI_REQ_RETRY_BATCH,      /* repeat the installs that failed */
    UI_REQ_MENU,             /* back to the menu */
    UI_REQ_RESTART,          /* restart the launcher */
} ui_req_t;

typedef void (*ui_request_cb_t)(ui_req_t req, int arg);

/* All functions take the (recursive) LVGL port lock themselves. */
void ui_init(ui_request_cb_t cb);

/* Menu: set the data first, then show. Cards = slots with a card, in `order`. */
void ui_set_apps(const basecamp_slot_t slots[APPS_MAX], const uint8_t order[APPS_MAX]);
void ui_set_hint(const char *text);          /* line under the title, e.g. the Wi-Fi name */
void ui_show_menu(void);
bool ui_menu_visible(void);
void ui_set_footer(const char *text);
void ui_show_launching(const char *name, uint32_t color);

/* Wi-Fi needed (GitHub downloads): how to add it in the web page. */
void ui_show_wifi_setup(const char *notice);

/* Install progress: one ring per slot in mask. */
void ui_show_progress(const char *title, uint32_t mask, const char *const names[APPS_MAX],
                      const uint32_t colors[APPS_MAX]);
void ui_progress_slot(int slot, int percent, const char *state, bool failed);
void ui_status(const char *text);
void ui_action_button(const char *label, ui_req_t req, int arg);
void ui_action_pair(const char *left, ui_req_t lreq, int larg, const char *right, ui_req_t rreq, int rarg);

/* Updates screen: one row per updatable app. */
void ui_show_updates(int n, const char *const names[], const uint32_t colors[]);
void ui_updates_row(int row, const char *text, bool available);
