/* Host unit tests for main/apps_logic.c. Run: tools/host_tests/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apps_logic.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define STREQ(a, b) do { checks++; if (strcmp((a), (b))) { fails++; printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static void test_order(void)
{
    uint8_t o[3], in[3];
    in[0] = 2; in[1] = 0; in[2] = 1; apps_normalize_order(in, o); CHECK(!memcmp(o, in, 3));
    in[0] = 1; in[1] = 1; in[2] = 9; apps_normalize_order(in, o); CHECK(o[0] == 1 && o[1] == 0 && o[2] == 2);
    in[0] = 0xFF; in[1] = 0xFF; in[2] = 0xFF; apps_normalize_order(in, o); CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2);

    uint8_t st[3] = {2, 0, 1}, out[3];
    CHECK(apps_build_order(st, 0x7, out) == 3 && out[0] == 2 && out[1] == 0 && out[2] == 1);
    CHECK(apps_build_order(st, 0x3, out) == 2 && out[0] == 0 && out[1] == 1);   /* slot 2 hidden */
    CHECK(apps_build_order(st, 0x4, out) == 1 && out[0] == 2);
    CHECK(apps_build_order(st, 0x0, out) == 0);

    uint8_t m[3] = {0, 1, 2};
    apps_order_move(m, 2, 0); CHECK(m[0] == 2 && m[1] == 0 && m[2] == 1);
    apps_order_move(m, 2, 2); CHECK(m[0] == 0 && m[1] == 1 && m[2] == 2);
    apps_order_move(m, 0, 1); CHECK(m[0] == 1 && m[1] == 0 && m[2] == 2);
    apps_order_move(m, 1, 99); CHECK(m[0] == 0 && m[1] == 2 && m[2] == 1);
    apps_order_move(m, 7, 0); CHECK(m[0] == 0 && m[1] == 2 && m[2] == 1);        /* bad slot: unchanged */
}

static void test_text(void)
{
    char b[16];
    CHECK(apps_sanitize_name("  Hello\tWorld \n", b, sizeof b) == 10); STREQ(b, "HelloWorld");
    CHECK(apps_sanitize_name("   ", b, sizeof b) == 0);
    apps_sanitize_name("0123456789abcdefXYZ", b, sizeof b); STREQ(b, "0123456789abcde");
    apps_sanitize_name("12345678901234\xC3\xA9", b, sizeof b);   /* é would be cut in half */
    CHECK(strlen(b) == 14);
    apps_prettify("printsphere_idf", b, sizeof b); STREQ(b, "Printsphere Idf");
    apps_prettify("arduino-lib-builder", b, sizeof b); STREQ(b, "Arduino Lib Bui");
    apps_prettify("__", b, sizeof b); STREQ(b, "App");

    char e[64];
    CHECK(apps_json_escape("a\"b\\c\n\x01", e, sizeof e) == 15); STREQ(e, "a\\\"b\\\\c\\n\\u0001");
    CHECK(apps_json_escape("0123456789", e, 5) == 10); STREQ(e, "0123");     /* truncated, still terminated */

    CHECK(apps_valid_repo("cptkirki/PrintSphere")); CHECK(apps_valid_repo("a/b.c-d_e"));
    CHECK(!apps_valid_repo("nodash")); CHECK(!apps_valid_repo("a/b/c")); CHECK(!apps_valid_repo("/ab"));
    CHECK(!apps_valid_repo("ab/")); CHECK(!apps_valid_repo("a b/c")); CHECK(!apps_valid_repo("a/b?x=1"));
    CHECK(apps_plain_text("abc DEF.bin", 20)); CHECK(!apps_plain_text("tab\there", 20));
    CHECK(!apps_plain_text("toolongtoolong", 5)); CHECK(!apps_plain_text("\xC3\xA9", 5));

    char d[32];
    CHECK(apps_urldecode("My+App%21%20x", 13, d, sizeof d) == 9); STREQ(d, "My App! x");
    apps_urldecode("100%", 4, d, sizeof d); STREQ(d, "100%");               /* dangling % kept */
    apps_urldecode("%4", 2, d, sizeof d); STREQ(d, "%4");
    CHECK(apps_parse_color("ff7a1a") == 0xFF7A1A); CHECK(apps_parse_color("#18C8B0") == 0x18C8B0);
    CHECK(apps_parse_color("fff") == -1); CHECK(apps_parse_color("gg0000") == -1);
}

static void make_hdr(uint8_t *h, unsigned chip, uint32_t desc_magic)
{
    memset(h, 0, 64);
    h[0] = 0xE9; h[1] = 5; h[12] = chip & 0xFF; h[13] = chip >> 8;
    h[32] = desc_magic & 0xFF; h[33] = (desc_magic >> 8) & 0xFF; h[34] = (desc_magic >> 16) & 0xFF; h[35] = desc_magic >> 24;
}

static void test_header(void)
{
    uint8_t h[64]; unsigned chip;
    make_hdr(h, 9, 0xABCD5432u); CHECK(apps_check_image_header(h, 64, &chip) == IMG_OK && chip == 9);
    CHECK(apps_check_image_header(h, 20, &chip) == IMG_TOO_SHORT);
    make_hdr(h, 5, 0xABCD5432u); CHECK(apps_check_image_header(h, 64, &chip) == IMG_WRONG_CHIP && chip == 5);
    STREQ(apps_img_check_msg(IMG_WRONG_CHIP, 5), "built for another chip (id 5), needs ESP32-S3");
    make_hdr(h, 9, 0x50505050u); CHECK(apps_check_image_header(h, 64, &chip) == IMG_NO_APP_DESC);
    make_hdr(h, 9, 0xABCD5432u); h[0] = 'P'; CHECK(apps_check_image_header(h, 64, &chip) == IMG_BAD_MAGIC);
    make_hdr(h, 9, 0xABCD5432u); h[1] = 0; CHECK(apps_check_image_header(h, 64, &chip) == IMG_BAD_SEGMENTS);
    make_hdr(h, 9, 0xABCD5432u); h[1] = 40; CHECK(apps_check_image_header(h, 64, &chip) == IMG_BAD_SEGMENTS);
}

static bool feed_chunks(const char *pat, bool nc, const uint8_t *data, size_t n, size_t chunk)
{
    apps_marker_t m; apps_marker_init(&m, pat, nc);
    for (size_t i = 0; i < n; i += chunk) apps_marker_feed(&m, data + i, (n - i < chunk) ? n - i : chunk);
    return m.found;
}

static void test_marker(void)
{
    const char *txt = "xxxCO53eCO5300yyy";
    for (size_t c = 1; c <= 20; c++) {                      /* every chunk size, incl. boundary splits */
        CHECK(feed_chunks("CO5300", false, (const uint8_t *)txt, strlen(txt), c));
        CHECK(!feed_chunks("CO5301", false, (const uint8_t *)txt, strlen(txt), c));
        CHECK(feed_chunks("co5300", true, (const uint8_t *)txt, strlen(txt), c));
        CHECK(!feed_chunks("co5300", false, (const uint8_t *)txt, strlen(txt), c));
    }
    CHECK(feed_chunks("aab", false, (const uint8_t *)"aaaab", 5, 1));           /* repeated prefix */
    CHECK(feed_chunks("abab", false, (const uint8_t *)"abaabab", 7, 2));
    CHECK(feed_chunks("", false, (const uint8_t *)"zzz", 3, 1));                /* empty => found */
    CHECK(!feed_chunks("esp32_s3_touch_amoled_1_75", false, (const uint8_t *)"esp32_s3_touch_lcd_2_8c", 23, 4));
}

/* Optional: real app images (tools/fetch_apps.sh puts them in apps/). */
static void test_real_images(const char *dir)
{
    const char *names[] = {"printsphere_ota-v1.6.2.bin", "CapsuleRadar-ota.bin", "lyricflash-ota.bin"};
    const char *markers[] = {"esp32_s3_touch_amoled_1_75", "CO5300", "esp32_s3_touch_amoled_1_75"};
    for (int i = 0; i < 3; i++) {
        char path[256]; snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb"); if (!f) { printf("  (skip %s)\n", names[i]); continue; }
        static uint8_t buf[5 << 20]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
        unsigned chip; CHECK(apps_check_image_header(buf, n, &chip) == IMG_OK);
        CHECK(feed_chunks(markers[i], false, buf, n, 1460));
        CHECK(feed_chunks("co5300", true, buf, n, 4096));
        printf("  real image %s ok\n", names[i]);
    }
}

int main(int argc, char **argv)
{
    test_order(); test_text(); test_header(); test_marker();
    if (argc > 1) test_real_images(argv[1]);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
