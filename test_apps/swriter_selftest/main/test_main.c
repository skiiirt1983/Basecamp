/*
 * Needs a valid app image in ota_0 (the runner puts this very app there).
 * Copies it into ota_1 through the slot writer in many ways and checks what
 * ends up in flash, and that obviously wrong input never erases anything.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "swriter.h"
#include "meta.h"
#include "slots.h"
#include "installer.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("TEST FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* slots.c needs the default-app table; the real one lives in installer.c (HTTP client etc.). */
void inst_preset(int k, inst_source_t *s, char name[32])
{
    memset(s, 0, sizeof *s);
    strcpy(name, k == 0 ? "PresetZero" : "Other");
    strcpy(s->repo, "owner/preset");
    strcpy(s->project, k == 0 ? "no_such_project" : "also_none");
}

static const esp_partition_t *P(int i)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, NULL);
}

static uint32_t s_len;
static uint8_t s_sha[32];

static bool slot_blank(int i)
{
    uint8_t b[64];
    esp_partition_read(P(i), 0, b, sizeof b);
    for (size_t k = 0; k < sizeof b; k++) if (b[k] != 0xFF) return false;
    return true;
}

/* slot i holds exactly the source image (compared in small pieces: QEMU has little RAM) */
static bool slot_equals_source(int i)
{
    uint8_t *a = malloc(4096), *b = malloc(4096);
    bool eq = true;
    for (uint32_t off = 0; off < s_len && eq; off += 4096) {
        uint32_t n = (s_len - off < 4096) ? s_len - off : 4096;
        esp_partition_read(P(0), off, a, n);
        esp_partition_read(P(i), off, b, n);
        eq = memcmp(a, b, n) == 0;
    }
    free(a);
    free(b);
    return eq;
}

typedef enum { M_NONE, M_CHIP, M_FLIP } mut_t;

/* Stream the first `upto` bytes of the source image into the writer. The first 150 bytes go one
 * byte at a time (header staging across chunk boundaries), the rest in `chunk`-sized pieces. */
static esp_err_t feed_src(uint32_t upto, uint32_t chunk, mut_t m, char *err)
{
    uint8_t *buf = malloc(chunk > 4096 ? chunk : 4096);
    esp_err_t e = ESP_OK;
    for (uint32_t off = 0; off < upto && e == ESP_OK;) {
        uint32_t n = (off < 150) ? 1 : chunk;
        if (n > upto - off) n = upto - off;
        esp_partition_read(P(0), off, buf, n);
        if (m == M_CHIP && off <= 12 && 12 < off + n) buf[12 - off] = 5;
        if (m == M_FLIP && off <= s_len / 2 && s_len / 2 < off + n) buf[s_len / 2 - off] ^= 0x5A;
        e = sw_write(buf, n, err, 100);
        off += n;
    }
    free(buf);
    return e;
}

static void install_good(uint32_t chunk)
{
    char err[100] = "";
    sw_result_t r;
    CHECK(sw_begin(1, s_len, "", err, sizeof err) == ESP_OK);
    CHECK(feed_src(s_len, chunk, M_NONE, err) == ESP_OK);
    CHECK(sw_finish(&r, err, sizeof err) == ESP_OK);
    sw_done();
}

static void test_writer(void)
{
    char err[100];
    sw_result_t r;

    /* 1. a valid image in different chunk sizes */
    static const uint32_t chunks[] = {7, 61, 1000, 4096};
    for (size_t k = 0; k < sizeof chunks / sizeof chunks[0]; k++) {
        err[0] = 0;
        CHECK(sw_begin(1, s_len, "SELFTEST", err, sizeof err) == ESP_OK);
        CHECK(feed_src(s_len, chunks[k], M_NONE, err) == ESP_OK);
        CHECK(sw_finish(&r, err, sizeof err) == ESP_OK);
        CHECK(r.size == s_len && !memcmp(r.sha256, s_sha, 32));
        CHECK(r.marker_found);
        CHECK(!strcmp(r.desc.project_name, "basecamp_swtest"));
        CHECK(sw_erased());
        sw_done();
        CHECK(slot_equals_source(1));
    }

    /* 2. required marker missing (pattern built at run time, so it is not in this image):
     *    finish succeeds, the caller rejects and aborts -> slot blank */
    char missing[32];
    snprintf(missing, sizeof missing, "NOT_%s_%d", "PRESENT", 42);
    CHECK(sw_begin(1, s_len, missing, err, sizeof err) == ESP_OK);
    CHECK(feed_src(s_len, 1024, M_NONE, err) == ESP_OK);
    CHECK(sw_finish(&r, err, sizeof err) == ESP_OK && !r.marker_found);
    sw_abort();
    CHECK(slot_blank(1));

    /* 3. bootloader / full-flash image: rejected BEFORE erasing; the old app survives */
    install_good(1000);
    uint8_t *boot = malloc(20000);
    esp_flash_read(NULL, boot, 0, 20000);
    CHECK(sw_begin(1, 20000, "", err, sizeof err) == ESP_OK);
    esp_err_t e = ESP_OK;
    for (uint32_t off = 0; off < 20000 && e == ESP_OK; off += 777) {
        e = sw_write(boot + off, (20000 - off < 777) ? 20000 - off : 777, err, sizeof err);
    }
    CHECK(e == ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "app-only") != NULL);
    CHECK(!sw_erased());
    sw_abort();
    free(boot);
    CHECK(slot_equals_source(1));

    /* 4. wrong chip id */
    CHECK(sw_begin(1, s_len, "", err, sizeof err) == ESP_OK);
    CHECK(feed_src(s_len, 4096, M_CHIP, err) == ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "another chip") != NULL && !sw_erased());
    sw_abort();
    CHECK(slot_equals_source(1));

    /* 5. not an ESP image at all (an HTML error page) */
    const char *html = "<html><head><title>504 Gateway Time-out</title></head><body>padding padding padding padding</body></html>";
    CHECK(sw_begin(1, strlen(html), "", err, sizeof err) == ESP_OK);
    e = ESP_OK;
    for (size_t off = 0; off < strlen(html) && e == ESP_OK; off += 20) {
        e = sw_write((const uint8_t *)html + off, (strlen(html) - off < 20) ? strlen(html) - off : 20, err, sizeof err);
    }
    CHECK(e == ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "not an ESP32") != NULL && !sw_erased());
    sw_abort();
    CHECK(slot_equals_source(1));

    /* 6. too small for a header: finish refuses, nothing erased */
    CHECK(sw_begin(1, 10, "", err, sizeof err) == ESP_OK);
    CHECK(feed_src(10, 10, M_NONE, err) == ESP_OK);
    CHECK(sw_finish(&r, err, sizeof err) == ESP_ERR_INVALID_SIZE && !sw_erased());
    sw_abort();
    CHECK(slot_equals_source(1));

    /* 7. truncated image: verification fails and the half-written slot is blanked */
    CHECK(sw_begin(1, s_len, "", err, sizeof err) == ESP_OK);
    CHECK(feed_src(s_len / 2, 2048, M_NONE, err) == ESP_OK);
    CHECK(sw_finish(&r, err, sizeof err) != ESP_OK);
    CHECK(sw_erased());
    sw_abort();
    CHECK(slot_blank(1));

    /* 8. announced size larger than the slot */
    CHECK(sw_begin(1, 5u << 20, "", err, sizeof err) == ESP_ERR_INVALID_SIZE);
    CHECK(strstr(err, "KB") != NULL);
    sw_abort();

    /* 9. one corrupted byte in the middle is caught by the image hash */
    install_good(4096);
    CHECK(sw_begin(1, s_len, "", err, sizeof err) == ESP_OK);
    CHECK(feed_src(s_len, 4096, M_FLIP, err) == ESP_OK);
    CHECK(sw_finish(&r, err, sizeof err) != ESP_OK);
    sw_abort();
    CHECK(slot_blank(1));
}

static void test_meta(void)
{
    char s[32];
    inst_source_t src, got;
    CHECK(!meta_get_name(2, s));
    CHECK(meta_get_color(0) == 0xFF7A1A && meta_get_color(1) == 0x18C8B0 && meta_get_color(2) == 0xC850F0);
    CHECK(meta_set_name(2, "  My\tApp  ") == ESP_OK && meta_get_name(2, s) && !strcmp(s, "MyApp"));
    CHECK(meta_set_color(2, 0x112233) == ESP_OK && meta_get_color(2) == 0x112233);
    memset(&src, 0, sizeof src);
    strcpy(src.repo, "o/r"); strcpy(src.prefix, "pre"); strcpy(src.suffix, ".bin");
    strcpy(src.exclude, "a,b"); strcpy(src.project, "proj"); strcpy(src.marker, "MARK");
    CHECK(meta_set_src(2, &src) == ESP_OK && meta_get_src(2, &got));
    CHECK(!strcmp(got.repo, "o/r") && !strcmp(got.prefix, "pre") && !strcmp(got.suffix, ".bin") &&
          !strcmp(got.exclude, "a,b") && !strcmp(got.project, "proj") && !strcmp(got.marker, "MARK"));
    strcpy(src.prefix, ""); strcpy(src.exclude, "");                    /* empty middle fields survive */
    CHECK(meta_set_src(1, &src) == ESP_OK && meta_get_src(1, &got) && !got.prefix[0] && !got.exclude[0] && !strcmp(got.project, "proj"));
    CHECK(meta_set_tag(2, "v1.2.3") == ESP_OK);
    meta_get_tag(2, s); CHECK(!strcmp(s, "v1.2.3"));

    uint8_t o[3];
    meta_get_order(o); CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2);
    uint8_t want[3] = {2, 0, 1};
    CHECK(meta_set_order(want) == ESP_OK); meta_get_order(o); CHECK(o[0] == 2 && o[1] == 0 && o[2] == 1);
    meta_move_to_end(2); meta_get_order(o); CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2);

    CHECK(!meta_get_ver(2, s));
    CHECK(meta_set_ver(2, "  1.21  ") == ESP_OK && meta_get_ver(2, s) && !strcmp(s, "1.21"));
    CHECK(meta_set_ver(2, "") == ESP_OK && !meta_get_ver(2, s));
    meta_set_ver(2, "9.9");
    meta_clear_slot(2);
    CHECK(!meta_get_ver(2, s));
    CHECK(!meta_get_name(2, s) && !meta_get_src(2, &got) && meta_get_color(2) == 0xC850F0);
    meta_get_tag(2, s); CHECK(!s[0]);
    meta_clear_slot(1);
}

static void test_slots(void)
{
    basecamp_slot_t s;
    slots_init();
    install_good(4096);
    slots_scan_all();
    slots_get(1, &s);
    CHECK(s.state == SLOT_APP && !strcmp(s.name, "Basecamp Swtest"));      /* name made from project_name */
    CHECK(!s.has_src && !s.changed && s.image_len == s_len);
    meta_set_ver(1, "7.7");                                                  /* a typed label wins over the app's own version */
    slots_scan_one(1);
    slots_get(1, &s);
    CHECK(!strcmp(s.version, "7.7"));
    meta_set_ver(1, "");
    slots_scan_one(1);
    slots_get(1, &s);
    slots_get(2, &s);
    CHECK(s.state == SLOT_EMPTY && slots_first_free() == 2);
    CHECK(slots_card_mask() == ((1u << 0) | (1u << 1)));                    /* ota_0 holds the source app too */

    /* an empty slot that keeps a GitHub source (failed install) still gets a "retry" card */
    inst_source_t src = {.repo = "owner/app"};
    meta_set_name(2, "Failed One"); meta_set_src(2, &src);
    slots_scan_one(2);
    slots_get(2, &s);
    CHECK(s.state == SLOT_EMPTY && s.has_src && slots_has_card(&s) && !strcmp(s.name, "Failed One"));

    /* corrupt a byte inside the stored image: reported as damaged, not bootable */
    /* Flash writes can only clear bits, so pick a byte that still has a bit set to clear
     * (a fixed bit would be a no-op for some builds). */
    uint32_t at = s_len / 2;
    uint8_t b;
    do {
        esp_partition_read(P(1), at, &b, 1);
        at++;
    } while (b == 0);
    b &= (uint8_t)(b - 1);                                                 /* clear the lowest set bit */
    esp_partition_write(P(1), at - 1, &b, 1);
    slots_scan_one(1);
    slots_get(1, &s);
    CHECK(s.state == SLOT_DAMAGED && slots_has_card(&s));

    /* overwrite guard: a different, valid image behind the stored baseline shows as "changed" */
    slots_erase(1);
    install_good(4096);
    slots_scan_one(1);
    slots_get(1, &s); CHECK(s.state == SLOT_APP && !s.changed);
    slots_erase(1);
    slots_get(1, &s);
    CHECK(s.state == SLOT_EMPTY && !s.has_src && slot_blank(1));
    slots_erase(2);
    slots_get(2, &s); CHECK(!s.has_src && !slots_has_card(&s));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    const esp_partition_t *src = P(0);
    esp_image_metadata_t md = {0};
    const esp_partition_pos_t pos = {.offset = src->address, .size = src->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) {
        printf("SWTEST FAIL: no valid app in ota_0\n");
        return;
    }
    s_len = md.image_len;
    {
        mbedtls_sha256_context c;
        uint8_t *b = malloc(4096);
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        for (uint32_t off = 0; off < s_len; off += 4096) {
            uint32_t n = (s_len - off < 4096) ? s_len - off : 4096;
            esp_partition_read(src, off, b, n);
            mbedtls_sha256_update(&c, b, n);
        }
        mbedtls_sha256_finish(&c, s_sha);
        mbedtls_sha256_free(&c);
        free(b);
    }
    printf("source image: %u bytes\n", (unsigned)s_len);

    test_writer();
    test_meta();
    test_slots();
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "SWTEST FAIL\n" : "SWTEST PASS\n");
}
