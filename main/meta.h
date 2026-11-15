/*
 * Per-slot metadata in NVS namespace "basecamp":
 *   name<i>  display name        col<i>  accent colour (u32 RGB)
 *   src<i>   GitHub source       tag<i>  installed release tag
 *   sha<i>   overwrite-guard baseline (see slots.c)
 *   ord      display order (3 bytes, a permutation of the slot numbers)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "apps_logic.h"

/* Where an app can be downloaded from: a GitHub repo's latest release. */
typedef struct {
    char repo[64];                      /* "owner/name" */
    char prefix[48];                    /* asset name starts with ... */
    char suffix[16];                    /* ... and ends with this */
    char exclude[32];                   /* comma-separated: skip assets whose name contains these */
    char project[32];                   /* installed project_name must contain this ("" = any) */
    char marker[APPS_MARKER_MAX + 1];   /* image must contain this string ("" = no check) */
} inst_source_t;

bool meta_get_name(int slot, char out[32]);
esp_err_t meta_set_name(int slot, const char *name);
uint32_t meta_get_color(int slot);                    /* stored colour or the slot default */
esp_err_t meta_set_color(int slot, uint32_t rgb);
bool meta_get_src(int slot, inst_source_t *out);
esp_err_t meta_set_src(int slot, const inst_source_t *src);   /* NULL clears */
void meta_get_tag(int slot, char out[32]);
esp_err_t meta_set_tag(int slot, const char *tag);            /* NULL or "" clears */
void meta_get_order(uint8_t ord[APPS_MAX]);                   /* always a valid permutation */
esp_err_t meta_set_order(const uint8_t ord[APPS_MAX]);
void meta_move_to_end(int slot);                              /* new apps appear last */
void meta_clear_slot(int slot);                               /* name, colour, source, tag, baseline */
