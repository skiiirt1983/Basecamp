/*
 * Slot scan. The menu is built from what is actually installed in the three
 * app slots (ota_0..2), plus per-slot metadata (meta.c).
 *
 * Overwrite guard (detection only): in-app OTA updaters write to the "next"
 * OTA slot, i.e. on top of a neighbouring app. The launcher stores each
 * image's SHA-256 in NVS the first time it sees it (or right after an
 * install); a different hash later marks the slot as changed.
 */
#include <string.h>
#include <stdio.h>
#include "slots.h"
#include "meta.h"
#include "installer.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "slots";

static basecamp_slot_t s_slots[APPS_MAX];
static SemaphoreHandle_t s_mtx;

void slots_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
    }
}

static const esp_partition_t *slot_part(int i)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, NULL);
}

/* Descriptors that say nothing about the actual application. */
static bool is_generic(const esp_app_desc_t *d)
{
    return strcmp(d->project_name, "arduino-lib-builder") == 0 || d->project_name[0] == '\0';
}

/* Streaming board-marker check over the stored image (chunked reads: no big MMU mapping). */
static bool image_has_marker(const esp_partition_t *part, uint32_t len, const char *marker)
{
    apps_marker_t m;
    apps_marker_init(&m, marker, false);
    if (m.found) return true;
    if (len == 0 || len > part->size) len = part->size;
    uint8_t *buf = malloc(4096);
    if (!buf) return true;                           /* cannot check: do not block */
    for (uint32_t off = 0; off < len && !m.found; off += 4096) {
        uint32_t n = (len - off < 4096) ? len - off : 4096;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) break;
        apps_marker_feed(&m, buf, n);
    }
    free(buf);
    return m.found;
}

static void check_baseline(int i, basecamp_slot_t *s, const uint8_t *now_in)
{
    uint8_t now[32], base[32];
    size_t len = sizeof(base);
    char key[8];
    nvs_handle_t h;

    if (now_in) {
        memcpy(now, now_in, sizeof(now));
    } else if (esp_partition_get_sha256(s->part, now) != ESP_OK) {
        return;
    }
    if (nvs_open("basecamp", NVS_READWRITE, &h) != ESP_OK) {
        return; /* NVS unavailable: no guard, but never block booting */
    }
    snprintf(key, sizeof(key), "sha%d", i);
    if (nvs_get_blob(h, key, base, &len) == ESP_OK && len == sizeof(base)) {
        s->changed = memcmp(now, base, sizeof(now)) != 0;
        if (s->changed) {
            ESP_LOGW(TAG, "%s: image changed since it was installed (in-app OTA?)", s->part->label);
        }
    } else if (nvs_set_blob(h, key, now, sizeof(now)) == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "%s: recorded baseline hash", s->part->label);
    }
    nvs_close(h);
}

/* Devices upgraded from 0.2.x have tags (and no names): keep their names and
 * update sources. Anything else gets a name made from its project_name. */
static void migrate_legacy(int i, basecamp_slot_t *s)
{
    char tag[32];
    meta_get_tag(i, tag);
    inst_source_t src;
    char pname[32];
    int adopt = -1;

    if (tag[0]) {
        inst_preset(i, &src, pname);
        if (s->generic_desc || strstr(s->desc.project_name, src.project)) adopt = i;
    }
    if (adopt < 0 && !s->generic_desc) {
        for (int k = 0; k < INST_PRESETS && adopt < 0; k++) {
            inst_preset(k, &src, pname);
            if (src.project[0] && strstr(s->desc.project_name, src.project)) adopt = k;
        }
    }
    if (adopt >= 0) {
        inst_preset(adopt, &src, pname);
        meta_set_name(i, pname);
        meta_set_src(i, &src);
        ESP_LOGI(TAG, "ota_%d: adopted default app '%s'", i, pname);
    } else {
        apps_prettify(s->desc.project_name, pname, sizeof pname);
        meta_set_name(i, pname);
    }
}

static void scan_into(int i, basecamp_slot_t *s)
{
    memset(s, 0, sizeof(*s));
    s->part = slot_part(i);
    s->color = meta_get_color(i);
    meta_get_tag(i, s->tag);
    inst_source_t src;
    s->has_src = meta_get_src(i, &src);
    bool named = meta_get_name(i, s->name);

    if (!s->part) {
        strlcpy(s->detail, "no partition", sizeof(s->detail));
        return;
    }
    if (esp_ota_get_partition_description(s->part, &s->desc) != ESP_OK) {
        if (!named) s->name[0] = 0;
        strlcpy(s->detail, s->has_src ? "failed" : "empty", sizeof(s->detail));
        return;                                      /* SLOT_EMPTY */
    }

    s->generic_desc = is_generic(&s->desc);
    const uint8_t *digest = NULL;
    esp_image_metadata_t md = {0};
    bool verified = true;
#if CONFIG_BASECAMP_VERIFY_SLOTS
    const esp_partition_pos_t pos = {.offset = s->part->address, .size = s->part->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) {
        verified = false;
    } else if (md.image.hash_appended) {
        digest = md.image_digest;
    }
    s->image_len = md.image_len;
#endif
    if (!named) {
        migrate_legacy(i, s);
        meta_get_name(i, s->name);
        s->has_src = meta_get_src(i, &src);
        meta_get_tag(i, s->tag);
    }
    if (s->name[0] == 0) apps_prettify(s->desc.project_name, s->name, sizeof(s->name));

    if (!verified) {
        s->state = SLOT_DAMAGED;
        strlcpy(s->detail, "damaged", sizeof(s->detail));
        ESP_LOGW(TAG, "%s: image failed verification", s->part->label);
        return;
    }
    /* Board check for apps that came from a source with a marker (e.g. PrintSphere's LCD 2.8C build). */
    if (s->has_src && src.marker[0] && !image_has_marker(s->part, s->image_len, src.marker)) {
        s->state = SLOT_DAMAGED;
        strlcpy(s->detail, "wrong board", sizeof(s->detail));
        ESP_LOGW(TAG, "%s: image is built for a different board", s->part->label);
        return;
    }

    s->state = SLOT_APP;
    check_baseline(i, s, digest);
    /* Version: the descriptor's, except for generic (Arduino) descriptors where the release tag is better. */
    if (s->generic_desc) {
        /* Arduino builds report a core build hash as the version: not meaningful, so use the release
         * tag if we have one, else nothing. */
        strlcpy(s->version, s->tag, sizeof(s->version));
    } else {
        strlcpy(s->version, s->desc.version, sizeof(s->version));
    }
    char label[32];
    if (meta_get_ver(i, label)) {                      /* a label typed on the web page wins */
        strlcpy(s->version, label, sizeof(s->version));
    }
    snprintf(s->detail, sizeof(s->detail), "%.31s", s->desc.project_name);
    ESP_LOGI(TAG, "%s @0x%06" PRIx32 ": %s %s (project '%s', IDF %s)", s->part->label, s->part->address,
             s->name, s->version, s->desc.project_name, s->desc.idf_ver);
}

void slots_scan_one(int i)
{
    basecamp_slot_t tmp;
    scan_into(i, &tmp);                              /* slow: flash reads + hashing, no lock held */
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    s_slots[i] = tmp;
    xSemaphoreGiveRecursive(s_mtx);
}

void slots_scan_all(void)
{
    for (int i = 0; i < APPS_MAX; i++) {
        slots_scan_one(i);
    }
}

void slots_get(int i, basecamp_slot_t *out)
{
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    *out = s_slots[i];
    xSemaphoreGiveRecursive(s_mtx);
}

void slots_get_all(basecamp_slot_t out[APPS_MAX])
{
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    memcpy(out, s_slots, sizeof(s_slots));
    xSemaphoreGiveRecursive(s_mtx);
}

bool slots_has_card(const basecamp_slot_t *s)
{
    return s->state == SLOT_APP || s->state == SLOT_DAMAGED || (s->state == SLOT_EMPTY && s->has_src);
}

unsigned slots_card_mask(void)
{
    basecamp_slot_t all[APPS_MAX];
    slots_get_all(all);
    unsigned m = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (slots_has_card(&all[i])) m |= 1u << i;
    }
    return m;
}

int slots_first_free(void)
{
    basecamp_slot_t all[APPS_MAX];
    slots_get_all(all);
    for (int i = 0; i < APPS_MAX; i++) {
        if (all[i].state == SLOT_EMPTY) return i;
    }
    return -1;
}

void slots_accept(int i)
{
    basecamp_slot_t s;
    slots_get(i, &s);
    uint8_t now[32];
    nvs_handle_t h;
    char key[8];
    if (!s.part || esp_partition_get_sha256(s.part, now) != ESP_OK ||
        nvs_open("basecamp", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    snprintf(key, sizeof(key), "sha%d", i);
    if (nvs_set_blob(h, key, now, sizeof(now)) == ESP_OK && nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "%s: new baseline accepted", s.part->label);
    }
    nvs_close(h);
    slots_scan_one(i);
}

esp_err_t slots_erase(int i)
{
    const esp_partition_t *p = slot_part(i);
    if (!p) return ESP_ERR_NOT_FOUND;
    esp_err_t e = esp_partition_erase_range(p, 0, 0x10000);
    meta_clear_slot(i);
    slots_scan_one(i);
    return e;
}
