/*
 * Installs an app straight from a GitHub release:
 *   GET https://api.github.com/repos/<repo>/releases/latest
 *   -> pick the asset by name -> stream it into the slot via the API asset URL
 *   (api.github.com/.../releases/assets/<id>, Accept: application/octet-stream,
 *   302 to release-assets.githubusercontent.com).
 * The github.com/.../releases/download route is only a fallback: it often
 * answers 504 after ~11 s, while the API route answers in <1 s.
 * The boot partition / otadata is never touched (the Basecamp bootloader
 * ignores it).
 */
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "installer.h"
#include "swriter.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "install";

#define API_FMT       "https://api.github.com/repos/%s/releases/latest"
#define API_MAX_BODY  (512 * 1024)
#define DL_CHUNK      4096
#define USER_AGENT    "Basecamp-launcher"

void inst_preset(int k, inst_source_t *src, char name[32])
{
    static const struct {
        const char *name, *repo, *prefix, *suffix, *exclude, *project, *marker;
    } p[INST_PRESETS] = {
        {CONFIG_BASECAMP_SLOT0_NAME, CONFIG_BASECAMP_SLOT0_REPO, CONFIG_BASECAMP_SLOT0_ASSET_PREFIX,
         CONFIG_BASECAMP_SLOT0_ASSET_SUFFIX, CONFIG_BASECAMP_SLOT0_ASSET_EXCLUDE,
         CONFIG_BASECAMP_SLOT0_PROJECT, CONFIG_BASECAMP_SLOT0_BOARD_MARKER},
        {CONFIG_BASECAMP_SLOT1_NAME, CONFIG_BASECAMP_SLOT1_REPO, CONFIG_BASECAMP_SLOT1_ASSET_PREFIX,
         CONFIG_BASECAMP_SLOT1_ASSET_SUFFIX, CONFIG_BASECAMP_SLOT1_ASSET_EXCLUDE,
         CONFIG_BASECAMP_SLOT1_PROJECT, CONFIG_BASECAMP_SLOT1_BOARD_MARKER},
        {CONFIG_BASECAMP_SLOT2_NAME, CONFIG_BASECAMP_SLOT2_REPO, CONFIG_BASECAMP_SLOT2_ASSET_PREFIX,
         CONFIG_BASECAMP_SLOT2_ASSET_SUFFIX, CONFIG_BASECAMP_SLOT2_ASSET_EXCLUDE,
         CONFIG_BASECAMP_SLOT2_PROJECT, CONFIG_BASECAMP_SLOT2_BOARD_MARKER},
    };
    memset(src, 0, sizeof *src);
    if (k < 0 || k >= INST_PRESETS) {
        name[0] = 0;
        return;
    }
    strlcpy(name, p[k].name, 32);
    strlcpy(src->repo, p[k].repo, sizeof src->repo);
    strlcpy(src->prefix, p[k].prefix, sizeof src->prefix);
    strlcpy(src->suffix, p[k].suffix, sizeof src->suffix);
    strlcpy(src->exclude, p[k].exclude, sizeof src->exclude);
    strlcpy(src->project, p[k].project, sizeof src->project);
    strlcpy(src->marker, p[k].marker, sizeof src->marker);
}

#define FAIL(code, ...) do { snprintf(err, err_len, __VA_ARGS__); \
                             ESP_LOGE(TAG, "%s", err); ret = (code); goto done; } while (0)

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
    char list[sizeof src->exclude];
    strlcpy(list, src->exclude, sizeof(list));
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
    size_t nl = strlen(name), pl = strlen(src->prefix), sl = strlen(src->suffix);
    return nl >= pl && nl >= sl && strncmp(name, src->prefix, pl) == 0 &&
           strcmp(name + nl - sl, src->suffix) == 0 && !excluded(src, name);
}

esp_err_t inst_fetch_latest(const inst_source_t *src, inst_release_t *out, char *err, size_t err_len)
{
    esp_err_t ret = ESP_OK;
    char *body = NULL;
    cJSON *root = NULL;
    esp_http_client_handle_t c = NULL;
    char url[sizeof src->repo + 48];

    memset(out, 0, sizeof(*out));
    if (!apps_valid_repo(src->repo)) {
        snprintf(err, err_len, "bad repo name");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(url, sizeof(url), API_FMT, src->repo);
    c = make_client(url, true);
    if (!c) FAIL(ESP_ERR_NO_MEM, "http client");

    int status = 0;
    int64_t clen = 0;
    if (open_follow(c, &status, &clen) != ESP_OK) FAIL(ESP_ERR_TIMEOUT, "GitHub unreachable");
    if (status == 403 || status == 429) FAIL(ESP_FAIL, "GitHub rate limit - try in an hour");
    if (status == 404) FAIL(ESP_ERR_NOT_FOUND, "%s has no release", src->repo);
    if (status >= 500) FAIL(ESP_ERR_TIMEOUT, "GitHub API HTTP %d", status);
    if (status != 200) FAIL(ESP_FAIL, "GitHub API HTTP %d", status);

    body = heap_caps_malloc(API_MAX_BODY + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) FAIL(ESP_ERR_NO_MEM, "no memory");
    int total = 0, r;
    while (total < API_MAX_BODY && (r = esp_http_client_read(c, body + total, API_MAX_BODY - total)) > 0) {
        total += r;
    }
    body[total] = 0;
    if (total <= 0) FAIL(ESP_ERR_TIMEOUT, "empty API response");

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
        const cJSON *api = cJSON_GetObjectItem(a, "url");
        const cJSON *size = cJSON_GetObjectItem(a, "size");
        const cJSON *digest = cJSON_GetObjectItem(a, "digest");
        if (cJSON_IsString(api)) {
            strlcpy(out->api_url, api->valuestring, sizeof(out->api_url));
        }
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
        ESP_LOGI(TAG, "%s %s, %s (%u bytes, digest %s)", src->repo, out->tag, out->asset_name,
                 (unsigned)out->size, out->has_digest ? "yes" : "no");
        goto done;
    }
    FAIL(ESP_ERR_NOT_FOUND, "%s %s has no matching .bin asset", src->repo, out->tag);

done:
    cJSON_Delete(root);
    free(body);
    if (c) {
        esp_http_client_cleanup(c);
    }
    return ret;
}

esp_err_t inst_install(int slot, const inst_source_t *src, const inst_release_t *rel,
                       inst_progress_cb_t cb, void *ctx, bool *slot_erased, char *err, size_t err_len)
{
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t c = NULL;
    uint8_t *buf = NULL;
    sw_result_t res;
    bool writer_open = false;
    *slot_erased = false;

    if (rel->size == 0) FAIL(ESP_ERR_INVALID_SIZE, "release asset is empty");
    buf = malloc(DL_CHUNK);
    if (!buf) FAIL(ESP_ERR_NO_MEM, "no memory");
    c = make_client(rel->api_url[0] ? rel->api_url : rel->url, false);
    if (!c) FAIL(ESP_ERR_NO_MEM, "http client");

    int status = 0;
    int64_t clen = 0;
    if (open_follow(c, &status, &clen) != ESP_OK) FAIL(ESP_ERR_TIMEOUT, "download failed to start");
    if (status >= 500 || status == 429) FAIL(ESP_ERR_TIMEOUT, "download HTTP %d", status);
    if (status != 200) FAIL(ESP_FAIL, "download HTTP %d", status);
    if (clen > 0 && (uint64_t)clen != rel->size)
        FAIL(ESP_ERR_INVALID_SIZE, "size mismatch (%ld vs %u)", (long)clen, (unsigned)rel->size);

    if ((ret = sw_begin(slot, rel->size, src->marker, err, err_len)) != ESP_OK) goto done;
    writer_open = true;

    uint32_t got = 0;
    int last_pct = -1;
    while (got < rel->size) {
        int r = esp_http_client_read(c, (char *)buf, DL_CHUNK);
        if (r < 0) FAIL(ESP_ERR_TIMEOUT, "download interrupted");
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(c)) {
                break;
            }
            FAIL(ESP_ERR_TIMEOUT, "download stalled");
        }
        if (got + r > rel->size) FAIL(ESP_ERR_INVALID_SIZE, "asset larger than announced");
        if ((ret = sw_write(buf, r, err, err_len)) != ESP_OK) goto done;
        got += r;
        int pct = (int)((uint64_t)got * 100 / rel->size);
        if (pct != last_pct && cb) {
            cb(slot, pct, ctx);
            last_pct = pct;
        }
    }
    if (got != rel->size) FAIL(ESP_ERR_TIMEOUT, "short download (%u/%u)", (unsigned)got, (unsigned)rel->size);

    if ((ret = sw_finish(&res, err, err_len)) != ESP_OK) goto done;
    if (rel->has_digest && memcmp(res.sha256, rel->sha256, 32) != 0) FAIL(ESP_ERR_INVALID_CRC, "sha256 mismatch");
    if (!res.marker_found) FAIL(ESP_ERR_INVALID_VERSION, "built for a different board");
    if (src->project[0] && !strstr(res.desc.project_name, src->project))
        FAIL(ESP_ERR_INVALID_VERSION, "wrong app: '%.31s' (expected '%.31s')", res.desc.project_name, src->project);

    ESP_LOGI(TAG, "ota_%d: installed %s %s (%s %s)", slot, src->repo, rel->tag, res.desc.project_name,
             res.desc.version);

done:
    if (writer_open) {
        *slot_erased = sw_erased();
        if (ret == ESP_OK) {
            sw_done();
        } else {
            sw_abort();
        }
    }
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(buf);
    return ret;
}
