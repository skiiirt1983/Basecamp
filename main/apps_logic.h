/*
 * Pure helpers for the dynamic app list (no ESP-IDF dependencies, so they are
 * unit-tested on the host: tools/host_tests/run.sh).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_MAX 3   /* the fixed partition table has three 4 MB app slots */

/* ---- display order ---- */
/* Make `in` a valid permutation of 0..APPS_MAX-1 (drop junk/duplicates, append
 * missing slots ascending). */
void apps_normalize_order(const uint8_t in[APPS_MAX], uint8_t out[APPS_MAX]);
/* Slots to show, in display order: members of `show_mask` following the stored
 * order. Returns how many were written to out[]. */
int apps_build_order(const uint8_t stored[APPS_MAX], unsigned show_mask, uint8_t out[APPS_MAX]);
/* Move `slot` to position `pos` (clamped) in a permutation. */
void apps_order_move(uint8_t ord[APPS_MAX], int slot, int pos);

/* ---- text ---- */
/* Trim and drop control characters; keeps UTF-8 bytes; returns length (0 = empty). */
size_t apps_sanitize_name(const char *in, char *out, size_t cap);
/* "printsphere_idf" -> "Printsphere Idf" */
void apps_prettify(const char *project, char *out, size_t cap);
/* JSON string escaping; always NUL-terminates; returns the full length needed. */
size_t apps_json_escape(const char *in, char *out, size_t cap);
/* "owner/name" using [A-Za-z0-9_.-] only. */
bool apps_valid_repo(const char *s);
/* Printable ASCII, no control characters, length < cap. */
bool apps_plain_text(const char *s, size_t max_len);
/* application/x-www-form-urlencoded / query decoding. Returns decoded length. */
size_t apps_urldecode(const char *in, size_t n, char *out, size_t cap);
/* "ff7a1a" or "#ff7a1a" -> 0xRRGGBB, or -1. */
long apps_parse_color(const char *s);

/* ---- ESP app image header pre-check (before anything is erased) ---- */
typedef enum {
    IMG_OK = 0,
    IMG_TOO_SHORT,
    IMG_BAD_MAGIC,      /* not an ESP image (zip? elf? html error page?) */
    IMG_WRONG_CHIP,     /* built for another chip */
    IMG_NO_APP_DESC,    /* bootloader / full-flash image, not an app-only .bin */
    IMG_BAD_SEGMENTS,
} img_check_t;

#define APPS_HDR_NEEDED 48   /* bytes of the file the check needs */
img_check_t apps_check_image_header(const uint8_t *d, size_t n, unsigned *chip_id);
const char *apps_img_check_msg(img_check_t r, unsigned chip_id);

/* ---- streaming substring search (KMP), optionally case-insensitive ---- */
#define APPS_MARKER_MAX 40
typedef struct {
    char pat[APPS_MARKER_MAX + 1];
    uint8_t fail[APPS_MARKER_MAX];
    uint8_t len, matched;
    bool nocase, found;
} apps_marker_t;

void apps_marker_init(apps_marker_t *m, const char *pat, bool nocase); /* empty pat => found */
void apps_marker_feed(apps_marker_t *m, const uint8_t *d, size_t n);
