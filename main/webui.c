/*
 * Web UI served on the open "BaseCamp-Setup" network (http://192.168.4.1, also
 * opened by the captive-portal redirect): upload / order / rename / remove
 * apps, install from GitHub, set the Wi-Fi used for GitHub downloads.
 *
 * Every POST must carry the header "X-Basecamp: 1". Browsers only allow a
 * cross-site page to send a custom header after a CORS preflight, which this
 * server never answers, so other web pages cannot drive the device.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "webui.h"
#include "ctl.h"
#include "slots.h"
#include "meta.h"
#include "installer.h"
#include "swriter.h"
#include "netmgr.h"
#include "apps_logic.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "cJSON.h"

static const char *TAG = "webui";

extern const uint8_t webui_html_start[] asm("_binary_webui_html_start");
extern const uint8_t webui_html_end[] asm("_binary_webui_html_end");

static httpd_handle_t s_httpd;

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(req, status, j);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, "200 OK", j);
}

static bool has_marker_header(httpd_req_t *req)
{
    return httpd_req_get_hdr_value_len(req, "X-Basecamp") > 0;
}

/* Reads a small JSON body; NULL on error (a response was already sent). */
static cJSON *read_json(httpd_req_t *req)
{
    if (!has_marker_header(req)) {
        send_err(req, "403 Forbidden", "missing X-Basecamp header");
        return NULL;
    }
    int len = req->content_len;
    if (len <= 0 || len > 2048) {
        send_err(req, "400 Bad Request", "bad request body");
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        send_err(req, "500 Internal Server Error", "no memory");
        return NULL;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) send_err(req, "400 Bad Request", "bad JSON");
    return j;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(o, k));
}

static bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[256], raw[160];
    out[0] = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, raw, sizeof raw) != ESP_OK) return false;
    apps_urldecode(raw, strlen(raw), out, cap);
    return true;
}

static void color_hex(uint32_t c, char out[8])
{
    snprintf(out, 8, "%06x", (unsigned)(c & 0xFFFFFF));
}

/* ---------- state ---------- */

static esp_err_t h_state(httpd_req_t *req)
{
    basecamp_slot_t all[APPS_MAX];
    slots_get_all(all);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "v", esp_app_get_description()->version);
    cJSON_AddBoolToObject(j, "busy", ctl_busy());

    cJSON *arr = cJSON_AddArrayToObject(j, "slots");
    int first_free = -1;
    for (int i = 0; i < APPS_MAX; i++) {
        const basecamp_slot_t *s = &all[i];
        inst_source_t src;
        char col[8];
        color_hex(s->color, col);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "st", s->state == SLOT_APP ? "app" : s->state == SLOT_DAMAGED ? "damaged" : "empty");
        cJSON_AddBoolToObject(o, "card", slots_has_card(s));
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddStringToObject(o, "ver", s->version);
        cJSON_AddStringToObject(o, "proj", s->desc.project_name);
        cJSON_AddStringToObject(o, "detail", s->detail);
        cJSON_AddStringToObject(o, "tag", s->tag);
        cJSON_AddStringToObject(o, "src", meta_get_src(i, &src) ? src.repo : "");
        cJSON_AddStringToObject(o, "col", col);
        cJSON_AddBoolToObject(o, "chg", s->changed);
        cJSON_AddNumberToObject(o, "size", s->image_len);
        cJSON_AddItemToArray(arr, o);
        if (first_free < 0 && s->state == SLOT_EMPTY) first_free = i;
    }
    cJSON_AddNumberToObject(j, "free", first_free);

    uint8_t ord[APPS_MAX];
    meta_get_order(ord);
    cJSON *oa = cJSON_AddArrayToObject(j, "order");
    for (int i = 0; i < APPS_MAX; i++) cJSON_AddItemToArray(oa, cJSON_CreateNumber(ord[i]));

    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    static const char *const st[] = {"none", "connecting", "connected", "failed"};
    cJSON_AddBoolToObject(w, "has", net_has_credentials());
    cJSON_AddStringToObject(w, "ssid", net_ssid());
    cJSON_AddStringToObject(w, "st", st[net_sta_state()]);
    char ip[16];
    cJSON_AddStringToObject(w, "ip", net_sta_ip(ip) ? ip : "");
    cJSON_AddBoolToObject(w, "ap", net_ap_active());
    cJSON_AddStringToObject(w, "why", net_sta_state() == NET_STA_FAILED ? net_fail_text() : "");

    ctl_job_t job;
    ctl_job_get(&job);
    cJSON *jo = cJSON_AddObjectToObject(j, "job");
    cJSON_AddBoolToObject(jo, "active", job.active);
    cJSON_AddNumberToObject(jo, "slot", job.slot);
    cJSON_AddNumberToObject(jo, "pct", job.pct);
    cJSON_AddStringToObject(jo, "text", job.text);
    cJSON_AddStringToObject(jo, "err", job.error);
    cJSON_AddNumberToObject(jo, "seq", job.seq);

    cJSON *pa = cJSON_AddArrayToObject(j, "presets");
    for (int k = 0; k < INST_PRESETS; k++) {
        inst_source_t p;
        char name[32];
        inst_preset(k, &p, name);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddStringToObject(o, "repo", p.repo);
        cJSON_AddStringToObject(o, "prefix", p.prefix);
        cJSON_AddStringToObject(o, "suffix", p.suffix);
        cJSON_AddStringToObject(o, "exclude", p.exclude);
        cJSON_AddStringToObject(o, "project", p.project);
        cJSON_AddStringToObject(o, "marker", p.marker);
        cJSON_AddItemToArray(pa, o);
    }
    return send_json(req, "200 OK", j);
}

/* ---------- upload ---------- */

static esp_err_t h_upload(httpd_req_t *req)
{
    if (!has_marker_header(req)) return send_err(req, "403 Forbidden", "missing X-Basecamp header");
    char tmp[64];
    int slot = -2;
    if (query_get(req, "slot", tmp, sizeof tmp)) slot = atoi(tmp);
    bool replace = query_get(req, "replace", tmp, sizeof tmp) && tmp[0] == '1';
    char name[32] = "";
    if (query_get(req, "name", tmp, sizeof tmp)) apps_sanitize_name(tmp, name, sizeof name);
    long color = -1;
    if (query_get(req, "color", tmp, sizeof tmp)) color = apps_parse_color(tmp);

    size_t len = req->content_len;
    if (len < APPS_HDR_NEEDED) return send_err(req, "400 Bad Request", "empty or too small file");
    if (len > 0x400000) return send_err(req, "413 Payload Too Large", "file is larger than the 4 MB slot");

    if (!ctl_begin()) return send_err(req, "409 Conflict", "the device is busy, try again in a moment");

    char err[100] = "";
    uint8_t *buf = NULL;
    const char *status = "400 Bad Request";
    bool sw_open = false, ok = false;
    sw_result_t res = {0};

    if (slot == -2 || slot == -1) slot = slots_first_free();
    if (slot < 0 || slot >= APPS_MAX) {
        status = "409 Conflict";
        snprintf(err, sizeof err, "no free slot: choose one to replace");
        goto done;
    }
    basecamp_slot_t old;
    slots_get(slot, &old);
    if (slots_has_card(&old) && !replace) {
        status = "409 Conflict";
        snprintf(err, sizeof err, "slot %d holds '%.31s': confirm to replace it", slot + 1, old.name);
        goto done;
    }
    buf = malloc(4096);
    if (!buf) {
        status = "500 Internal Server Error";
        snprintf(err, sizeof err, "no memory");
        goto done;
    }
    if (sw_begin(slot, len, "", err, sizeof err) != ESP_OK) goto done;
    sw_open = true;

    ESP_LOGI(TAG, "upload %u bytes -> slot %d", (unsigned)len, slot);
    ctl_upload_progress(slot, 0);
    size_t got = 0;
    int timeouts = 0, last = -1;
    while (got < len) {
        size_t want = len - got < 4096 ? len - got : 4096;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) {
            status = "408 Request Timeout";
            snprintf(err, sizeof err, "upload interrupted");
            goto done;
        }
        timeouts = 0;
        if (sw_write(buf, r, err, sizeof err) != ESP_OK) goto done;
        got += r;
        int pct = (int)((uint64_t)got * 100 / len);
        if (pct != last) {
            ctl_upload_progress(slot, pct);
            last = pct;
        }
    }
    if (sw_finish(&res, err, sizeof err) != ESP_OK) goto done;

    /* Accepted: the slot now holds a custom app with no GitHub source. */
    meta_clear_slot(slot);
    char shown[32];
    if (name[0]) strlcpy(shown, name, sizeof shown);
    else apps_prettify(res.desc.project_name, shown, sizeof shown);
    meta_set_name(slot, shown);
    if (color >= 0) meta_set_color(slot, (uint32_t)color);
    meta_move_to_end(slot);
    sw_done();
    sw_open = false;
    ok = true;

done:
    if (sw_open) {
        if (sw_erased()) meta_clear_slot(slot);      /* the old app is gone: forget its metadata */
        sw_abort();
    }
    free(buf);
    esp_err_t rc;
    if (ok) {
        slots_scan_one(slot);
        basecamp_slot_t now;
        slots_get(slot, &now);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddNumberToObject(j, "slot", slot);
        cJSON_AddStringToObject(j, "name", now.name);
        if (!res.display_seen) {
            cJSON_AddStringToObject(j, "warn", "No CO5300 display driver found in this image: it may not be built for this board.");
        }
        rc = send_json(req, "200 OK", j);
    } else {
        if (slot >= 0 && slot < APPS_MAX) slots_scan_one(slot);
        rc = send_err(req, status, err[0] ? err : "upload failed");
    }
    ctl_end();
    ctl_upload_finished();
    return rc;
}

/* ---------- metadata ---------- */

static esp_err_t h_order(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    const cJSON *a = cJSON_GetObjectItem(j, "order");
    uint8_t ord[APPS_MAX] = {0xFF, 0xFF, 0xFF};
    int n = cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0;
    for (int i = 0; i < n && i < APPS_MAX; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (cJSON_IsNumber(v)) ord[i] = (uint8_t)v->valuedouble;
    }
    cJSON_Delete(j);
    if (n != APPS_MAX) return send_err(req, "400 Bad Request", "order must list all slots");
    if (!ctl_begin()) return send_err(req, "409 Conflict", "the device is busy");
    meta_set_order(ord);
    ctl_end();
    ctl_menu_changed();
    return send_ok(req);
}

static esp_err_t h_rename(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    int slot = jint(j, "slot", -1);
    char name[32];
    apps_sanitize_name(jstr(j, "name"), name, sizeof name);
    long color = apps_parse_color(jstr(j, "color"));
    cJSON_Delete(j);
    if (slot < 0 || slot >= APPS_MAX) return send_err(req, "400 Bad Request", "bad slot");
    if (!ctl_begin()) return send_err(req, "409 Conflict", "the device is busy");
    if (name[0]) meta_set_name(slot, name);
    if (color >= 0) meta_set_color(slot, (uint32_t)color);
    slots_scan_one(slot);
    ctl_end();
    ctl_menu_changed();
    return send_ok(req);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    int slot = jint(j, "slot", -1);
    cJSON_Delete(j);
    if (slot < 0 || slot >= APPS_MAX) return send_err(req, "400 Bad Request", "bad slot");
    if (!ctl_begin()) return send_err(req, "409 Conflict", "the device is busy");
    esp_err_t e = slots_erase(slot);
    ctl_end();
    ctl_menu_changed();
    return e == ESP_OK ? send_ok(req) : send_err(req, "500 Internal Server Error", "erase failed");
}

/* ---------- GitHub install ---------- */

static bool take_field(const cJSON *j, const char *k, char *dst, size_t cap, size_t max_len, char *err, size_t errn)
{
    const char *v = jstr(j, k);
    if (!apps_plain_text(v, max_len) || strlen(v) >= cap) {
        snprintf(err, errn, "field '%s' has odd characters or is too long", k);
        return false;
    }
    strlcpy(dst, v, cap);
    return true;
}

static esp_err_t h_install(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    char err[100] = "", name[32] = "";
    inst_source_t src;
    memset(&src, 0, sizeof src);
    int slot = jint(j, "slot", -1);
    int upd = jint(j, "update", -1);
    bool replace = jbool(j, "replace");
    long color = apps_parse_color(jstr(j, "color"));
    const char *status = "400 Bad Request";
    esp_err_t rc;

    if (upd >= 0) {                                   /* update / reinstall from the stored source */
        if (upd >= APPS_MAX || !meta_get_src(upd, &src)) {
            snprintf(err, sizeof err, "this app has no GitHub source");
            goto bad;
        }
        slot = upd;
        replace = true;
        meta_get_name(upd, name);
        color = -1;
    } else {
        if (!take_field(j, "repo", src.repo, sizeof src.repo, 63, err, sizeof err) ||
            !take_field(j, "prefix", src.prefix, sizeof src.prefix, 47, err, sizeof err) ||
            !take_field(j, "suffix", src.suffix, sizeof src.suffix, 15, err, sizeof err) ||
            !take_field(j, "exclude", src.exclude, sizeof src.exclude, 31, err, sizeof err) ||
            !take_field(j, "project", src.project, sizeof src.project, 31, err, sizeof err) ||
            !take_field(j, "marker", src.marker, sizeof src.marker, APPS_MARKER_MAX, err, sizeof err)) {
            goto bad;
        }
        if (!apps_valid_repo(src.repo)) {
            snprintf(err, sizeof err, "repository must look like owner/name");
            goto bad;
        }
        apps_sanitize_name(jstr(j, "name"), name, sizeof name);
        if (!name[0]) strlcpy(name, strchr(src.repo, '/') + 1, sizeof name);
        if (slot < 0) slot = slots_first_free();
        if (slot < 0 || slot >= APPS_MAX) {
            status = "409 Conflict";
            snprintf(err, sizeof err, "no free slot: choose one to replace");
            goto bad;
        }
        basecamp_slot_t old;
        slots_get(slot, &old);
        if (slots_has_card(&old) && !replace) {
            status = "409 Conflict";
            snprintf(err, sizeof err, "slot %d holds '%.31s': confirm to replace it", slot + 1, old.name);
            goto bad;
        }
    }
    if (ctl_start_install(slot, &src, name, color >= 0 ? (uint32_t)color : meta_get_color(slot)) != ESP_OK) {
        status = "409 Conflict";
        snprintf(err, sizeof err, "the device is busy, try again in a moment");
        goto bad;
    }
    cJSON_Delete(j);
    return send_ok(req);
bad:
    cJSON_Delete(j);
    rc = send_err(req, status, err);
    return rc;
}

/* ---------- Wi-Fi ---------- */

static esp_err_t h_wifi(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    char ssid[33], pass[65];
    strlcpy(ssid, jstr(j, "ssid"), sizeof ssid);
    strlcpy(pass, jstr(j, "pass"), sizeof pass);
    cJSON_Delete(j);
    if (!ssid[0]) return send_err(req, "400 Bad Request", "network name is empty");
    if (pass[0] && strlen(pass) < 8) return send_err(req, "400 Bad Request", "Wi-Fi passwords are at least 8 characters");
    if (net_set_credentials(ssid, pass) != ESP_OK) return send_err(req, "500 Internal Server Error", "could not save");
    esp_err_t rc = send_ok(req);                      /* answer first: the setup network may go away */
    ctl_wifi_changed();
    return rc;
}

static esp_err_t h_wifi_forget(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    cJSON_Delete(j);
    net_forget_credentials();
    esp_err_t rc = send_ok(req);
    ctl_wifi_changed();
    return rc;
}

static esp_err_t h_scan_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    cJSON_Delete(j);
    return net_scan_start() == ESP_OK ? send_ok(req) : send_err(req, "500 Internal Server Error", "scan failed");
}

static esp_err_t h_scan_get(httpd_req_t *req)
{
    net_ap_info_t nets[16];
    int n = net_scan_running() ? 0 : net_scan_results(nets, 16);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "running", net_scan_running());
    cJSON *a = cJSON_AddArrayToObject(j, "nets");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(o, "open", nets[i].open);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(req, "200 OK", j);
}

/* ---------- page + captive portal ---------- */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL byte */
    return httpd_resp_send(req, (const char *)webui_html_start, webui_html_end - webui_html_start - 1);
}

/* Anything else (OS captive-portal probes included) -> redirect to the page. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    if (!net_ap_active()) {                           /* on the home network there is no captive portal */
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" NET_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t webui_start(void)
{
    if (s_httpd) return ESP_OK;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;
    hc.max_open_sockets = 6;
    hc.max_uri_handlers = 14;
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 15;
    hc.send_wait_timeout = 15;
    esp_err_t e = httpd_start(&s_httpd, &hc);
    if (e != ESP_OK) return e;
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/api/state", .method = HTTP_GET, .handler = h_state},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/order", .method = HTTP_POST, .handler = h_order},
        {.uri = "/api/rename", .method = HTTP_POST, .handler = h_rename},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/install", .method = HTTP_POST, .handler = h_install},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/wifi/forget", .method = HTTP_POST, .handler = h_wifi_forget},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = h_scan_post},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_scan_get},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "web UI up");
    return ESP_OK;
}

void webui_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}
