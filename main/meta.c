#include <stdio.h>
#include <string.h>
#include "meta.h"
#include "nvs.h"

#define NS "basecamp"

static const uint32_t s_default_color[APPS_MAX] = {0xFF7A1A, 0x18C8B0, 0xC850F0};

static void key(char *k, size_t n, const char *base, int slot)
{
    snprintf(k, n, "%s%d", base, slot);
}

static bool get_str(const char *k, char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = cap;
    bool ok = nvs_get_str(h, k, out, &l) == ESP_OK;
    nvs_close(h);
    if (!ok) out[0] = 0;
    return ok;
}

static esp_err_t set_str(const char *k, const char *v)   /* v == NULL: erase */
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = v ? nvs_set_str(h, k, v) : nvs_erase_key(h, k);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool meta_get_name(int slot, char out[32])
{
    char k[12];
    key(k, sizeof k, "name", slot);
    return get_str(k, out, 32) && out[0];
}

esp_err_t meta_set_name(int slot, const char *name)
{
    char k[12], clean[32];
    key(k, sizeof k, "name", slot);
    if (!name || !apps_sanitize_name(name, clean, sizeof clean)) return set_str(k, NULL);
    return set_str(k, clean);
}

bool meta_get_ver(int slot, char out[32])
{
    char k[12];
    key(k, sizeof k, "ver", slot);
    return get_str(k, out, 32) && out[0];
}

esp_err_t meta_set_ver(int slot, const char *ver)
{
    char k[12], clean[32];
    key(k, sizeof k, "ver", slot);
    if (!ver || !apps_sanitize_name(ver, clean, sizeof clean)) return set_str(k, NULL);
    return set_str(k, clean);
}

uint32_t meta_get_color(int slot)
{
    nvs_handle_t h;
    uint32_t v = s_default_color[(slot >= 0 && slot < APPS_MAX) ? slot : 0];
    char k[12];
    key(k, sizeof k, "col", slot);
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t c;
        if (nvs_get_u32(h, k, &c) == ESP_OK && c <= 0xFFFFFF) v = c;
        nvs_close(h);
    }
    return v;
}

esp_err_t meta_set_color(int slot, uint32_t rgb)
{
    nvs_handle_t h;
    char k[12];
    key(k, sizeof k, "col", slot);
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u32(h, k, rgb & 0xFFFFFF);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* "repo\tprefix\tsuffix\texclude\tproject\tmarker" */
bool meta_get_src(int slot, inst_source_t *out)
{
    char k[12], buf[320];
    key(k, sizeof k, "src", slot);
    if (!get_str(k, buf, sizeof buf) || !buf[0]) return false;
    memset(out, 0, sizeof(*out));
    char *f[6] = {0};
    int n = 0;
    for (char *p = buf; n < 6;) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    if (!f[0] || !apps_valid_repo(f[0])) return false;
    strlcpy(out->repo, f[0], sizeof out->repo);
    if (n > 1) strlcpy(out->prefix, f[1], sizeof out->prefix);
    if (n > 2) strlcpy(out->suffix, f[2], sizeof out->suffix);
    if (n > 3) strlcpy(out->exclude, f[3], sizeof out->exclude);
    if (n > 4) strlcpy(out->project, f[4], sizeof out->project);
    if (n > 5) strlcpy(out->marker, f[5], sizeof out->marker);
    return true;
}

esp_err_t meta_set_src(int slot, const inst_source_t *s)
{
    char k[12], buf[320];
    key(k, sizeof k, "src", slot);
    if (!s) return set_str(k, NULL);
    snprintf(buf, sizeof buf, "%s\t%s\t%s\t%s\t%s\t%s", s->repo, s->prefix, s->suffix, s->exclude,
             s->project, s->marker);
    return set_str(k, buf);
}

void meta_get_tag(int slot, char out[32])
{
    char k[12];
    key(k, sizeof k, "tag", slot);
    get_str(k, out, 32);
}

esp_err_t meta_set_tag(int slot, const char *tag)
{
    char k[12];
    key(k, sizeof k, "tag", slot);
    return set_str(k, (tag && tag[0]) ? tag : NULL);
}

void meta_get_order(uint8_t ord[APPS_MAX])
{
    uint8_t raw[APPS_MAX] = {0xFF, 0xFF, 0xFF};
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof raw;
        nvs_get_blob(h, "ord", raw, &l);
        nvs_close(h);
    }
    apps_normalize_order(raw, ord);
}

esp_err_t meta_set_order(const uint8_t ord[APPS_MAX])
{
    uint8_t full[APPS_MAX];
    nvs_handle_t h;
    apps_normalize_order(ord, full);
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "ord", full, sizeof full);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void meta_move_to_end(int slot)
{
    uint8_t o[APPS_MAX];
    meta_get_order(o);
    apps_order_move(o, slot, APPS_MAX - 1);
    meta_set_order(o);
}

void meta_clear_slot(int slot)
{
    char k[12];
    static const char *const bases[] = {"name", "col", "src", "tag", "sha", "ver"};
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        key(k, sizeof k, bases[i], slot);
        nvs_erase_key(h, k);
    }
    nvs_commit(h);
    nvs_close(h);
}
