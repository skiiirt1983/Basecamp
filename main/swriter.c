#include <stdio.h>
#include <string.h>
#include "swriter.h"
#include "apps_logic.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

static const char *TAG = "swriter";

#define HDR_STAGE 64
#define BLANK_BYTES 0x10000

static struct {
    bool active, begun, erased;
    const esp_partition_t *part;
    esp_ota_handle_t ota;
    uint8_t hdr[HDR_STAGE];
    size_t hdr_len;
    uint32_t total;
    mbedtls_sha256_context sha;
    apps_marker_t required, display;
} s;

static void fail(char *err, size_t n, const char *msg)
{
    snprintf(err, n, "%s", msg);
    ESP_LOGW(TAG, "%s", msg);
}

esp_err_t sw_begin(int slot, uint32_t expected_size, const char *required_marker, char *err, size_t errlen)
{
    if (s.active) sw_abort();
    memset(&s, 0, sizeof s);
    s.part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot, NULL);
    if (!s.part) {
        fail(err, errlen, "no such app slot");
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > s.part->size) {
        snprintf(err, errlen, "image is %u KB, a slot holds %u KB", (unsigned)(expected_size / 1024),
                 (unsigned)(s.part->size / 1024));
        return ESP_ERR_INVALID_SIZE;
    }
    mbedtls_sha256_init(&s.sha);
    mbedtls_sha256_starts(&s.sha, 0);
    apps_marker_init(&s.required, required_marker ? required_marker : "", false);
    apps_marker_init(&s.display, "co5300", true);
    s.active = true;
    return ESP_OK;
}

static esp_err_t begin_flash(char *err, size_t errlen)
{
    unsigned chip;
    img_check_t c = apps_check_image_header(s.hdr, s.hdr_len, &chip);
    if (c != IMG_OK) {
        fail(err, errlen, apps_img_check_msg(c, chip));
        return ESP_ERR_INVALID_ARG;
    }
    /* Sequential mode erases sector by sector while writing: no long freeze of the display. */
    if (esp_ota_begin(s.part, OTA_WITH_SEQUENTIAL_WRITES, &s.ota) != ESP_OK) {
        fail(err, errlen, "flash erase failed");
        return ESP_FAIL;
    }
    s.begun = s.erased = true;
    if (esp_ota_write(s.ota, s.hdr, s.hdr_len) != ESP_OK) {
        fail(err, errlen, "flash write failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sw_write(const uint8_t *d, size_t n, char *err, size_t errlen)
{
    if (!s.active) return ESP_ERR_INVALID_STATE;
    if ((uint64_t)s.total + n > s.part->size) {
        fail(err, errlen, "image is larger than the 4 MB slot");
        return ESP_ERR_INVALID_SIZE;
    }
    mbedtls_sha256_update(&s.sha, d, n);
    apps_marker_feed(&s.required, d, n);
    apps_marker_feed(&s.display, d, n);
    s.total += n;

    if (!s.begun) {                                  /* stage the first bytes for the header check */
        size_t take = HDR_STAGE - s.hdr_len;
        if (take > n) take = n;
        memcpy(s.hdr + s.hdr_len, d, take);
        s.hdr_len += take;
        d += take;
        n -= take;
        if (s.hdr_len < HDR_STAGE) return ESP_OK;
        esp_err_t e = begin_flash(err, errlen);
        if (e != ESP_OK) return e;
    }
    if (n && esp_ota_write(s.ota, d, n) != ESP_OK) {
        fail(err, errlen, "flash write failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sw_finish(sw_result_t *res, char *err, size_t errlen)
{
    if (!s.active) return ESP_ERR_INVALID_STATE;
    if (!s.begun) {
        fail(err, errlen, "file too small to be an app image");
        return ESP_ERR_INVALID_SIZE;
    }
    s.begun = false;                                 /* esp_ota_end consumes the handle */
    if (esp_ota_end(s.ota) != ESP_OK) {
        fail(err, errlen, "image failed verification (incomplete, corrupt or wrong chip)");
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    memset(res, 0, sizeof *res);
    if (esp_ota_get_partition_description(s.part, &res->desc) != ESP_OK) {
        fail(err, errlen, "no app descriptor");
        return ESP_ERR_NOT_FOUND;
    }
    res->size = s.total;
    mbedtls_sha256_finish(&s.sha, res->sha256);
    res->marker_found = s.required.found;
    res->display_seen = s.display.found;
    return ESP_OK;
}

void sw_abort(void)
{
    if (!s.active) return;
    if (s.begun) {
        esp_ota_abort(s.ota);
    }
    if (s.erased) {                                  /* never leave half an image behind */
        esp_partition_erase_range(s.part, 0, BLANK_BYTES);
    }
    mbedtls_sha256_free(&s.sha);
    s.active = false;
}

void sw_done(void)
{
    if (!s.active) return;
    mbedtls_sha256_free(&s.sha);
    s.active = false;
}

bool sw_erased(void) { return s.erased; }
uint32_t sw_received(void) { return s.total; }
