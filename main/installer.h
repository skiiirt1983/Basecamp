#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "basecamp_bootsel.h"

/* Per-slot app source (from Kconfig: GitHub repo, asset name pattern and the
 * project_name the installed image must carry). */
typedef struct {
    const char *repo;          /* "owner/repo" */
    const char *asset_prefix;  /* asset name must start with this ... */
    const char *asset_suffix;  /* ... and end with this */
    const char *project_match; /* esp_app_desc_t.project_name must contain this */
    const char *asset_exclude; /* comma-separated substrings: skip matching asset names */
    const char *board_marker;  /* image must contain this byte string ("" = no check) */
} inst_source_t;

const inst_source_t *inst_source(int slot);

typedef struct {
    char tag[32];
    char asset_name[96];
    char url[256];             /* browser_download_url (fallback) */
    char api_url[160];         /* api.github.com/.../releases/assets/<id> (preferred) */
    uint32_t size;
    bool has_digest;
    uint8_t sha256[32];
} inst_release_t;

/* Errors worth retrying (HTTP 5xx/429, network) come back as ESP_ERR_TIMEOUT. */

/* GET /repos/<repo>/releases/latest and pick the slot's asset. */
esp_err_t inst_fetch_latest(int slot, inst_release_t *out, char *err, size_t err_len);

/* percent 0..100 */
typedef void (*inst_progress_cb_t)(int slot, int percent, void *ctx);

/* Download the asset into ota_<slot> (esp_ota_begin/write/end on that
 * partition; never touches the boot partition), verify size, sha256 digest
 * and project_name, then store the tag and re-baseline the overwrite guard.
 * On failure the slot is left erased (shows as empty). */
esp_err_t inst_install(int slot, const inst_release_t *rel, inst_progress_cb_t cb, void *ctx,
                       char *err, size_t err_len);

/* Board check: does the app image at the start of part (len bytes) contain
 * the slot's board marker? True when no marker is configured. */
bool inst_image_has_marker(int slot, const esp_partition_t *part, uint32_t len);

/* Release tag installed by the launcher ("" if unknown). */
void inst_get_tag(int slot, char *out, size_t len);
