#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "meta.h"

/* The built-in "default apps" (Kconfig: menuconfig -> Basecamp launcher -> Default apps).
 * They are only a convenience list; any slot can hold any app. */
#define INST_PRESETS 3
void inst_preset(int k, inst_source_t *src, char name[32]);

typedef struct {
    char tag[32];
    char asset_name[96];
    char url[256];             /* browser_download_url (fallback) */
    char api_url[160];         /* api.github.com/.../releases/assets/<id> (preferred) */
    uint32_t size;
    bool has_digest;
    uint8_t sha256[32];
} inst_release_t;

/* Errors worth retrying (HTTP 5xx/429, network) are returned as ESP_ERR_TIMEOUT. */

/* GET /repos/<repo>/releases/latest and pick the source's asset. */
esp_err_t inst_fetch_latest(const inst_source_t *src, inst_release_t *out, char *err, size_t err_len);

/* percent 0..100 */
typedef void (*inst_progress_cb_t)(int slot, int percent, void *ctx);

/* Download the asset into ota_<slot> through the slot writer, check size, SHA-256
 * digest (when GitHub gives one), board marker and project_name. The caller stores
 * the metadata (name, source, release tag). *slot_erased tells whether the slot's old content is gone. On
 * failure the slot is blanked if it was erased. */
esp_err_t inst_install(int slot, const inst_source_t *src, const inst_release_t *rel,
                       inst_progress_cb_t cb, void *ctx, bool *slot_erased, char *err, size_t err_len);
