#include <string.h>
#include <stdio.h>
#include "slots.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "nvs.h"

/*
 * Overwrite guard (detection only): each in-app OTA updater writes to the
 * "next" OTA slot, i.e. on top of a neighbouring app. The first time the
 * launcher sees a valid image in a slot it stores the image SHA-256 in NVS
 * (namespace "basecamp"); afterwards a different hash marks the slot as
 * changed. tools/flash.sh erases NVS, so a fresh flash re-baselines.
 */
#define NVS_NS "basecamp"

static const char *TAG = "slots";

static const char *const s_cfg_names[BASECAMP_NUM_SLOTS] = {
    CONFIG_BASECAMP_SLOT0_NAME, CONFIG_BASECAMP_SLOT1_NAME, CONFIG_BASECAMP_SLOT2_NAME,
};
static const char *const s_cfg_versions[BASECAMP_NUM_SLOTS] = {
    CONFIG_BASECAMP_SLOT0_VERSION, CONFIG_BASECAMP_SLOT1_VERSION, CONFIG_BASECAMP_SLOT2_VERSION,
};

/* project_name in esp_app_desc_t -> friendly name. Names are taken from the
 * image itself, so the menu reflects what is *really* in a slot (e.g. after an
 * in-app OTA overwrote a neighbour). */
static const struct {
    const char *project;
    const char *label;
} s_known[] = {
    {"printsphere_idf", "PrintSphere"},
    {"printsphere", "PrintSphere"},
    {"lyricflash", "LyricFlash"},
    {"capsule_radar", "Capsule Radar"},
};

/* now: image SHA-256 (from esp_image_verify), or NULL to compute it */
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
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return; /* NVS unavailable: no guard, but never block booting */
    }
    snprintf(key, sizeof(key), "sha%d", i);
    if (nvs_get_blob(h, key, base, &len) == ESP_OK && len == sizeof(base)) {
        s->changed = memcmp(now, base, sizeof(now)) != 0;
        if (s->changed) {
            ESP_LOGW(TAG, "%s: image changed since it was flashed (in-app OTA?)", s->part->label);
        }
    } else if (nvs_set_blob(h, key, now, sizeof(now)) == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "%s: recorded baseline hash", s->part->label);
    }
    nvs_close(h);
}

void slots_accept(basecamp_slot_t *s, int i)
{
    uint8_t now[32];
    char key[8];
    nvs_handle_t h;
    if (!s->part || esp_partition_get_sha256(s->part, now) != ESP_OK ||
        nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    snprintf(key, sizeof(key), "sha%d", i);
    if (nvs_set_blob(h, key, now, sizeof(now)) == ESP_OK && nvs_commit(h) == ESP_OK) {
        s->changed = false;
        ESP_LOGI(TAG, "%s: new baseline accepted", s->part->label);
    }
    nvs_close(h);
}

/* Descriptors that say nothing about the actual application. */
static bool is_generic(const esp_app_desc_t *d)
{
    return strcmp(d->project_name, "arduino-lib-builder") == 0 ||
           d->project_name[0] == '\0';
}

static void scan_one(int i, basecamp_slot_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->name, s_cfg_names[i], sizeof(s->name));

    s->part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                       ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, NULL);
    if (!s->part) {
        strlcpy(s->detail, "no partition", sizeof(s->detail));
        return;
    }

    if (esp_ota_get_partition_description(s->part, &s->desc) != ESP_OK) {
        strlcpy(s->detail, "empty", sizeof(s->detail));
        ESP_LOGI(TAG, "%s: empty / no app descriptor", s->part->label);
        return;
    }

    const uint8_t *digest = NULL;
#if CONFIG_BASECAMP_VERIFY_SLOTS
    esp_image_metadata_t md = {0};
    const esp_partition_pos_t pos = {.offset = s->part->address, .size = s->part->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) {
        strlcpy(s->detail, "invalid image", sizeof(s->detail));
        ESP_LOGW(TAG, "%s: image failed verification", s->part->label);
        return;
    }
    if (md.image.hash_appended) {
        digest = md.image_digest;
    }
#endif

    s->valid = true;
    s->generic_desc = is_generic(&s->desc);
    check_baseline(i, s, digest);

    if (s->generic_desc) {
        /* Keep the configured name; prefer a configured version label. */
        if (s_cfg_versions[i][0]) {
            snprintf(s->version, sizeof(s->version), "%s*", s_cfg_versions[i]);
        } else {
            strlcpy(s->version, s->desc.version, sizeof(s->version));
        }
        snprintf(s->detail, sizeof(s->detail), "Arduino / IDF %.20s", s->desc.idf_ver);
    } else {
        const char *label = s->desc.project_name;
        for (size_t k = 0; k < sizeof(s_known) / sizeof(s_known[0]); k++) {
            if (strcmp(s->desc.project_name, s_known[k].project) == 0) {
                label = s_known[k].label;
                break;
            }
        }
        strlcpy(s->name, label, sizeof(s->name));
        strlcpy(s->version, s->desc.version, sizeof(s->version));
        snprintf(s->detail, sizeof(s->detail), "%.31s", s->desc.project_name);
    }

    ESP_LOGI(TAG, "%s @0x%06" PRIx32 ": %s %s (project '%s', IDF %s, %s %s)",
             s->part->label, s->part->address, s->name, s->version,
             s->desc.project_name, s->desc.idf_ver, s->desc.date, s->desc.time);
}

void slots_scan(basecamp_slot_t slots[BASECAMP_NUM_SLOTS])
{
    for (int i = 0; i < BASECAMP_NUM_SLOTS; i++) {
        scan_one(i, &slots[i]);
    }
}
