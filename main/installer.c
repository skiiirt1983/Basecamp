/*
 * Installs app slots straight from GitHub releases:
 *   GET https://api.github.com/repos/<repo>/releases/latest
 *   -> pick the slot's asset -> stream browser_download_url (302 to
 *   release-assets.githubusercontent.com) into the OTA partition.
 * Only esp_ota_begin/write/end on the target partition is used; the boot
 * partition / otadata is never changed (the Basecamp bootloader ignores it).
 */
#include <string.h>
#include <stdio.h>
#include "installer.h"
#include "slots.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const char *TAG = "install";

#define NVS_NS        "basecamp"
#define API_FMT       "https://api.github.com/repos/%s/releases/latest"
#define API_MAX_BODY  (512 * 1024)
#define DL_CHUNK      4096
#define USER_AGENT    "SLOPEFORM-Basecamp-launcher"

static const inst_source_t s_sources[BASECAMP_NUM_SLOTS] = {
    {CONFIG_BASECAMP_SLOT0_REPO, CONFIG_BASECAMP_SLOT0_ASSET_PREFIX,
     CONFIG_BASECAMP_SLOT0_ASSET_SUFFIX, CONFIG_BASECAMP_SLOT0_PROJECT,
     CONFIG_BASECAMP_SLOT0_ASSET_EXCLUDE, CONFIG_BASECAMP_SLOT0_BOARD_MARKER},
    {CONFIG_BASECAMP_SLOT1_REPO, CONFIG_BASECAMP_SLOT1_ASSET_PREFIX,
     CONFIG_BASECAMP_SLOT1_ASSET_SUFFIX, CONFIG_BASECAMP_SLOT1_PROJECT,
     CONFIG_BASECAMP_SLOT1_ASSET_EXCLUDE, CONFIG_BASECAMP_SLOT1_BOARD_MARKER},
    {CONFIG_BASECAMP_SLOT2_REPO, CONFIG_BASECAMP_SLOT2_ASSET_PREFIX,
     CONFIG_BASECAMP_SLOT2_ASSET_SUFFIX, CONFIG_BASECAMP_SLOT2_PROJECT,
     CONFIG_BASECAMP_SLOT2_ASSET_EXCLUDE, CONFIG_BASECAMP_SLOT2_BOARD_MARKER},
};

const inst_source_t *inst_source(int slot)
{
    return (slot >= 0 && slot < BASECAMP_NUM_SLOTS) ? &s_sources[slot] : NULL;
}

#define FAIL(code, ...) do { snprintf(err, err_len, __VA_ARGS__); \
                             ESP_LOGE(TAG, "ota_%d: %s", slot, err); ret = (code); goto done; } while (0)

static bool redirect_status(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

/* Open url, following redirects, until a non-redirect status arrives. */
static esp_err_t open_follow(esp_http_client_handle_t c, int *status, int64_t *len)
{
    for (int hop = 0; hop < 6; hop++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            return e;
        }
        *len = esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (!redirect_status(*status)) {
            return ESP_OK;
        }
        if (esp_http_client_set_redirection(c) != ESP_OK) {
            return ESP_FAIL;
        }
        char drain[256];
        while (esp_http_client_read(c, drain, sizeof(drain)) > 0) {
        }
    }
    return ESP_ERR_HTTP_MAX_REDIRECT;
}

static esp_http_client_handle_t make_client(const char *url, bool api)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,      /* long signed Location headers on redirect */
        .buffer_size_tx = 2048,   /* long redirected request lines */
        .max_redirection_count = 6,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "User-Agent", USER_AGENT);
        if (api) {
            esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
            esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");
        } else {
            esp_http_client_set_header(c, "Accept", "application/octet-stream");
        }
    }
    return c;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool excluded(const inst_source_t *src, const char *name)
{
    char list[64];
    strlcpy(list, src->asset_exclude, sizeof(list));
    for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        if (*tok && strcasestr(name, tok)) {
            return true;
        }
    }
    return false;
}

static bool name_matches(const inst_source_t *src, const char *name)
{
    size_t nl = strlen(name), pl = strlen(src->asset_prefix), sl = strlen(src->asset_suffix);
    return nl >= pl && nl >= sl && strncmp(name, src->asset_prefix, pl) == 0 &&
           strcmp(name + nl - sl, src->asset_suffix) == 0 && !excluded(src, name);
}

/* Streaming substring search across chunk boundaries. */
typedef struct {
    const char *pat;
    size_t len, matched;
    bool found;
} marker_t;

static void marker_init(marker_t *m, const char *pat)
{
    m->pat = pat;
    m->len = strlen(pat);
    m->matched = 0;
    m->found = (m->len == 0);
}

static void marker_feed(marker_t *m, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n && !m->found; i++) {
        /* Naive restart is fine: the markers have no repeated prefix. */
        if (d[i] == (uint8_t)m->pat[m->matched]) {
            if (++m->matched == m->len) {
                m->found = true;
            }
        } else {
            m->matched = (d[i] == (uint8_t)m->pat[0]) ? 1 : 0;
        }
    }
}

bool inst_image_has_marker(int slot, const esp_partition_t *part, uint32_t len)
{
    const inst_source_t *src = inst_source(slot);
    marker_t m;
    marker_init(&m, src ? src->board_marker : "");
    if (m.found) {
        return true;
    }
    if (len == 0 || len > part->size) {
        len = part->size;
    }
    const void *map;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(part, 0, len, ESP_PARTITION_MMAP_DATA, &map, &h) != ESP_OK) {
        return true; /* cannot check - do not block */
    }
    marker_feed(&m, map, len);
    esp_partition_munmap(h);
    return m.found;
}

esp_err_t inst_fetch_latest(int slot, inst_release_t *out, char *err, size_t err_len)
{
    const inst_source_t *src = inst_source(slot);
    esp_err_t ret = ESP_OK;
    char *body = NULL;
    cJSON *root = NULL;
    esp_http_client_handle_t c = NULL;
    char url[160];

    memset(out, 0, sizeof(*out));
    if (!src) {
        snprintf(err, err_len, "bad slot");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(url, sizeof(url), API_FMT, src->repo);
    c = make_client(url, true);
    if (!c) FAIL(ESP_ERR_NO_MEM, "http client");

    int status = 0;
    int64_t clen = 0;
    if (open_follow(c, &status, &clen) != ESP_OK) FAIL(ESP_FAIL, "GitHub unreachable");
    if (status == 403 || status == 429) FAIL(ESP_FAIL, "GitHub rate limit - try later");
    if (status == 404) FAIL(ESP_ERR_NOT_FOUND, "%s has no release", src->repo);
    if (status != 200) FAIL(ESP_FAIL, "GitHub API HTTP %d", status);

    body = heap_caps_malloc(API_MAX_BODY + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) FAIL(ESP_ERR_NO_MEM, "no memory");
    int total = 0, r;
    while (total < API_MAX_BODY && (r = esp_http_client_read(c, body + total, API_MAX_BODY - total)) > 0) {
        total += r;
    }
    body[total] = 0;
    if (total <= 0) FAIL(ESP_FAIL, "empty API response");

    root = cJSON_Parse(body);
    if (!root) FAIL(ESP_FAIL, "bad API response");
    const cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    const cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsString(tag) || !cJSON_IsArray(assets)) FAIL(ESP_FAIL, "release has no assets");
    strlcpy(out->tag, tag->valuestring, sizeof(out->tag));

    const cJSON *a;
    cJSON_ArrayForEach(a, assets) {
        const cJSON *name = cJSON_GetObjectItem(a, "name");
        if (!cJSON_IsString(name) || !name_matches(src, name->valuestring)) {
            continue;
        }
        const cJSON *dl = cJSON_GetObjectItem(a, "browser_download_url");
        const cJSON *size = cJSON_GetObjectItem(a, "size");
        const cJSON *digest = cJSON_GetObjectItem(a, "digest");
        if (!cJSON_IsString(dl) || !cJSON_IsNumber(size)) {
            continue;
        }
        strlcpy(out->asset_name, name->valuestring, sizeof(out->asset_name));
        strlcpy(out->url, dl->valuestring, sizeof(out->url));
        out->size = (uint32_t)size->valuedouble;
        if (cJSON_IsString(digest) && strncmp(digest->valuestring, "sha256:", 7) == 0 &&
            strlen(digest->valuestring) == 7 + 64) {
            out->has_digest = hex_to_bytes(digest->valuestring + 7, out->sha256, 32);
        }
        ESP_LOGI(TAG, "ota_%d: %s %s, %s (%" PRIu32 " bytes, digest %s)", slot, src->repo,
                 out->tag, out->asset_name, out->size, out->has_digest ? "yes" : "no");
        goto done;
    }
    FAIL(ESP_ERR_NOT_FOUND, "%s %s has no %s*%s asset", src->repo, out->tag,
         src->asset_prefix, src->asset_suffix);

done:
    cJSON_Delete(root);
    free(body);
    if (c) {
        esp_http_client_cleanup(c);
    }
    return ret;
}

static void save_tag(int slot, const char *tag)
{
    nvs_handle_t h;
    char key[8];
    snprintf(key, sizeof(key), "tag%d", slot);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (tag) {
            nvs_set_str(h, key, tag);
        } else {
            nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

void inst_get_tag(int slot, char *out, size_t len)
{
    nvs_handle_t h;
    char key[8];
    out[0] = 0;
    snprintf(key, sizeof(key), "tag%d", slot);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            out[0] = 0;
        }
        nvs_close(h);
    }
}

esp_err_t inst_install(int slot, const inst_release_t *rel, inst_progress_cb_t cb, void *ctx,
                       char *err, size_t err_len)
{
    const inst_source_t *src = inst_source(slot);
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t c = NULL;
    esp_ota_handle_t ota = 0;
    bool ota_open = false, slot_touched = false;
    uint8_t *buf = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot, NULL);
    if (!src || !part) FAIL(ESP_ERR_NOT_FOUND, "no ota_%d partition", slot);
    if (part == esp_ota_get_running_partition()) FAIL(ESP_FAIL, "slot is running");
    if (rel->size == 0 || rel->size > part->size)
        FAIL(ESP_ERR_INVALID_SIZE, "%s is %" PRIu32 " KB, slot holds %" PRIu32 " KB",
             rel->asset_name, rel->size / 1024, part->size / 1024);

    buf = malloc(DL_CHUNK);
    if (!buf) FAIL(ESP_ERR_NO_MEM, "no memory");
    c = make_client(rel->url, false);
    if (!c) FAIL(ESP_ERR_NO_MEM, "http client");

    int status = 0;
    int64_t clen = 0;
    if (open_follow(c, &status, &clen) != ESP_OK) FAIL(ESP_FAIL, "download failed to start");
    if (status != 200) FAIL(ESP_FAIL, "download HTTP %d", status);
    if (clen > 0 && (uint64_t)clen != rel->size)
        FAIL(ESP_ERR_INVALID_SIZE, "size mismatch (%ld vs %" PRIu32 ")", (long)clen, rel->size);

    ESP_LOGI(TAG, "ota_%d: writing %s to %s @0x%06" PRIx32, slot, rel->asset_name, part->label, part->address);
    slot_touched = true;
    /* Sequential mode erases sector by sector while writing: no multi-second
     * full-slot erase that would freeze the display. */
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) FAIL(ESP_FAIL, "flash erase failed");
    ota_open = true;
    mbedtls_sha256_starts(&sha, 0);

    uint32_t got = 0;
    int last_pct = -1;
    marker_t board;
    marker_init(&board, src->board_marker);
    while (got < rel->size) {
        int r = esp_http_client_read(c, (char *)buf, DL_CHUNK);
        if (r < 0) FAIL(ESP_FAIL, "download interrupted");
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(c)) {
                break;
            }
            FAIL(ESP_FAIL, "download stalled");
        }
        if (got + r > rel->size) FAIL(ESP_ERR_INVALID_SIZE, "asset larger than announced");
        mbedtls_sha256_update(&sha, buf, r);
        marker_feed(&board, buf, r);
        if (esp_ota_write(ota, buf, r) != ESP_OK) FAIL(ESP_FAIL, "flash write failed");
        got += r;
        int pct = (int)((uint64_t)got * 100 / rel->size);
        if (pct != last_pct && cb) {
            cb(slot, pct, ctx);
            last_pct = pct;
        }
    }
    if (got != rel->size) FAIL(ESP_ERR_INVALID_SIZE, "short download (%" PRIu32 "/%" PRIu32 ")", got, rel->size);

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    if (rel->has_digest && memcmp(digest, rel->sha256, 32) != 0) FAIL(ESP_ERR_INVALID_CRC, "sha256 mismatch");
    if (!board.found) FAIL(ESP_ERR_INVALID_VERSION, "built for a different board");

    ota_open = false;
    if (esp_ota_end(ota) != ESP_OK) FAIL(ESP_ERR_OTA_VALIDATE_FAILED, "image failed verification");

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) FAIL(ESP_FAIL, "no app descriptor");
    if (!strstr(desc.project_name, src->project_match))
        FAIL(ESP_FAIL, "wrong app: '%s' (expected '%s')", desc.project_name, src->project_match);

    save_tag(slot, rel->tag);
    basecamp_slot_t s = {.part = part};
    slots_accept(&s, slot); /* new image is the guard's baseline */
    ESP_LOGI(TAG, "ota_%d: installed %s %s (%s %s)", slot, src->repo, rel->tag, desc.project_name, desc.version);

done:
    if (ota_open) {
        esp_ota_abort(ota);
    }
    if (ret != ESP_OK && slot_touched && part) {
        /* Never leave a half-written image: blank the header so the slot reads as empty. */
        esp_partition_erase_range(part, 0, 0x10000);
        save_tag(slot, NULL);
    }
    mbedtls_sha256_free(&sha);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(buf);
    return ret;
}
