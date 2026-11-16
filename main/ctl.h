/* Controller interface (implemented in main.c) used by the web UI. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "meta.h"

typedef struct {
    bool active;
    int slot;            /* -1 when none */
    int pct;
    char text[64];
    char error[96];      /* result of the last finished job ("" = ok) */
    uint32_t seq;        /* changes whenever a job finishes */
} ctl_job_t;

/* Only one operation at a time (upload, install, delete, ...). */
bool ctl_begin(void);        /* true: you now own it; false: busy */
void ctl_end(void);
bool ctl_busy(void);
void ctl_job_get(ctl_job_t *out);

/* Metadata or slots changed: rescan and redraw the menu if it is showing. */
void ctl_menu_changed(void);

/* Why the launcher last started, e.g. "power-on (before: brown-out)". */
const char *ctl_boot_info(void);

/* Wi-Fi credentials were saved or forgotten: join the home network and switch the setup
 * network off (or bring it back when there is no usable saved network). Returns at once. */
void ctl_wifi_changed(void);

/* Upload progress on the device screen. */
void ctl_upload_progress(int slot, int pct);
void ctl_upload_finished(void);

/* Start a GitHub install in the background (takes the busy state itself).
 * Returns ESP_ERR_INVALID_STATE when busy. */
esp_err_t ctl_start_install(int slot, const inst_source_t *src, const char *name, uint32_t color);
