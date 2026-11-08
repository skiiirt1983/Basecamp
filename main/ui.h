#pragma once
#include "slots.h"

/* Callback when a slot is chosen; called from the LVGL task. */
typedef void (*ui_launch_cb_t)(int slot);

/* Must be called with the LVGL port lock held. */
/* Long-press on a "changed" slot: accept its current image as the baseline. */
typedef void (*ui_accept_cb_t)(int slot);

void ui_create(const basecamp_slot_t slots[BASECAMP_NUM_SLOTS], ui_launch_cb_t launch_cb,
               ui_accept_cb_t accept_cb);
void ui_set_footer(const char *text);
