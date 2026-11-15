#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apps_logic.h"

/* ---------- order ---------- */

void apps_normalize_order(const uint8_t in[APPS_MAX], uint8_t out[APPS_MAX])
{
    unsigned seen = 0;
    int n = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (in[i] < APPS_MAX && !(seen & (1u << in[i]))) {
            seen |= 1u << in[i];
            out[n++] = in[i];
        }
    }
    for (uint8_t s = 0; s < APPS_MAX; s++) {
        if (!(seen & (1u << s))) {
            out[n++] = s;
        }
    }
}

int apps_build_order(const uint8_t stored[APPS_MAX], unsigned show_mask, uint8_t out[APPS_MAX])
{
    uint8_t full[APPS_MAX];
    apps_normalize_order(stored, full);
    int n = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (show_mask & (1u << full[i])) {
            out[n++] = full[i];
        }
    }
    return n;
}

void apps_order_move(uint8_t ord[APPS_MAX], int slot, int pos)
{
    uint8_t full[APPS_MAX], res[APPS_MAX];
    apps_normalize_order(ord, full);
    if (slot < 0 || slot >= APPS_MAX) {
        memcpy(ord, full, APPS_MAX);
        return;
    }
    if (pos < 0) pos = 0;
    if (pos > APPS_MAX - 1) pos = APPS_MAX - 1;
    int n = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (full[i] != slot) {
            res[n++] = full[i];
        }
    }
    int w = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        ord[i] = (i == pos) ? (uint8_t)slot : res[w++];
    }
}

/* ---------- text ---------- */

size_t apps_sanitize_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (cap == 0) {
        return 0;
    }
    while (*in && isspace((unsigned char)*in)) {
        in++;
    }
    for (; *in && o + 1 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        out[o++] = (char)c;
    }
    while (o > 0 && isspace((unsigned char)out[o - 1])) {
        o--;
    }
    /* do not cut a UTF-8 sequence in half */
    if (*in && o > 0) {
        size_t k = o;
        while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0 && (unsigned char)out[k - 1] >= 0xC0) o = k - 1;
    }
    out[o] = 0;
    return o;
}

void apps_prettify(const char *project, char *out, size_t cap)
{
    size_t o = 0;
    bool word_start = true;
    for (; *project && o + 1 < cap; project++) {
        char c = *project;
        if (c == '_' || c == '-' || c == '.') {
            if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
            word_start = true;
            continue;
        }
        out[o++] = word_start ? (char)toupper((unsigned char)c) : c;
        word_start = false;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (o == 0 && cap > 4) {
        strcpy(out, "App");
    }
}

size_t apps_json_escape(const char *in, char *out, size_t cap)
{
    size_t need = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        char tmp[8];
        size_t l;
        switch (c) {
        case '"':  strcpy(tmp, "\\\""); l = 2; break;
        case '\\': strcpy(tmp, "\\\\"); l = 2; break;
        case '\n': strcpy(tmp, "\\n"); l = 2; break;
        case '\r': strcpy(tmp, "\\r"); l = 2; break;
        case '\t': strcpy(tmp, "\\t"); l = 2; break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                tmp[0] = '\\'; tmp[1] = 'u'; tmp[2] = '0'; tmp[3] = '0';
                tmp[4] = hex[c >> 4]; tmp[5] = hex[c & 15];
                l = 6;
            } else {
                tmp[0] = (char)c; l = 1;
            }
        }
        if (cap && need + l < cap) {
            memcpy(out + need, tmp, l);
        }
        need += l;
    }
    if (cap) {
        out[need < cap ? need : cap - 1] = 0;
    }
    return need;
}

bool apps_valid_repo(const char *s)
{
    size_t slash = 0, len = strlen(s);
    if (len < 3 || len > 63) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '/') {
            if (i == 0 || i == len - 1 || ++slash > 1) return false;
        } else if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')) {
            return false;
        }
    }
    return slash == 1;
}

bool apps_plain_text(const char *s, size_t max_len)
{
    size_t n = strlen(s);
    if (n > max_len) return false;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) return false;
    }
    return true;
}

size_t apps_urldecode(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (in[i] == '+') {
            out[o++] = ' ';
        } else if (in[i] == '%' && i + 2 < n &&
                   isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char hx[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
    return o;
}

long apps_parse_color(const char *s)
{
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    long v = 0;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) return -1;
        char hx[2] = {s[i], 0};
        v = v * 16 + strtol(hx, NULL, 16);
    }
    return v;
}

/* ---------- ESP image header ---------- */

img_check_t apps_check_image_header(const uint8_t *d, size_t n, unsigned *chip_id)
{
    if (chip_id) *chip_id = 0;
    if (n < APPS_HDR_NEEDED) return IMG_TOO_SHORT;
    if (d[0] != 0xE9) return IMG_BAD_MAGIC;
    unsigned chip = d[12] | (d[13] << 8);
    if (chip_id) *chip_id = chip;
    if (chip != 9) return IMG_WRONG_CHIP;                      /* ESP32-S3 */
    if (d[1] == 0 || d[1] > 16) return IMG_BAD_SEGMENTS;
    /* first segment header at 24, esp_app_desc_t magic at 32 */
    uint32_t magic = d[32] | (d[33] << 8) | (d[34] << 16) | ((uint32_t)d[35] << 24);
    if (magic != 0xABCD5432u) return IMG_NO_APP_DESC;
    return IMG_OK;
}

const char *apps_img_check_msg(img_check_t r, unsigned chip_id)
{
    static char buf[80];
    switch (r) {
    case IMG_OK: return "ok";
    case IMG_TOO_SHORT: return "file too small to be an app image";
    case IMG_BAD_MAGIC: return "not an ESP32 .bin (zip, elf or an error page?)";
    case IMG_WRONG_CHIP:
        snprintf(buf, sizeof(buf), "built for another chip (id %u), needs ESP32-S3", chip_id);
        return buf;
    case IMG_NO_APP_DESC: return "no app descriptor: use the app-only OTA .bin, not a merged/full-flash or bootloader image";
    case IMG_BAD_SEGMENTS: return "corrupt image header";
    }
    return "invalid image";
}

/* ---------- streaming marker search (KMP) ---------- */

static uint8_t fold(uint8_t c, bool nocase)
{
    return nocase ? (uint8_t)tolower(c) : c;
}

void apps_marker_init(apps_marker_t *m, const char *pat, bool nocase)
{
    memset(m, 0, sizeof(*m));
    size_t l = strlen(pat);
    if (l > APPS_MARKER_MAX) l = APPS_MARKER_MAX;
    memcpy(m->pat, pat, l);
    m->len = (uint8_t)l;
    m->nocase = nocase;
    m->found = (l == 0);
    for (size_t i = 0; i < l; i++) m->pat[i] = (char)fold((uint8_t)m->pat[i], nocase);
    /* failure function */
    uint8_t k = 0;
    for (size_t i = 1; i < l; i++) {
        while (k > 0 && m->pat[i] != m->pat[k]) k = m->fail[k - 1];
        if (m->pat[i] == m->pat[k]) k++;
        m->fail[i] = k;
    }
}

void apps_marker_feed(apps_marker_t *m, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n && !m->found; i++) {
        uint8_t c = fold(d[i], m->nocase);
        while (m->matched > 0 && (char)c != m->pat[m->matched]) {
            m->matched = m->fail[m->matched - 1];
        }
        if ((char)c == m->pat[m->matched]) {
            m->matched++;
        }
        if (m->matched == m->len) {
            m->found = true;
        }
    }
}
