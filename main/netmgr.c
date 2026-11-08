/*
 * Launcher Wi-Fi: station connect with NVS-stored credentials and a captive
 * setup portal (SoftAP + catch-all DNS + one-page HTTP form).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "netmgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

#define NVS_NS      "basecamp"
#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
#define BIT_PORTAL_OK BIT2
#define MAX_RETRY   4
#define MAX_APS     20

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta, *s_ap;
static bool s_inited, s_portal, s_started;
static int s_retry;
static char s_ssid[33], s_pass[65];

static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static int s_dns_sock = -1;
static wifi_ap_record_t s_aps[MAX_APS];
static uint16_t s_ap_count;

/* ---------- credentials ---------- */

static void load_creds(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t l = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &l) != ESP_OK) {
        s_ssid[0] = 0;
    }
    l = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &l) != ESP_OK) {
        s_pass[0] = 0;
    }
    nvs_close(h);
}

static esp_err_t save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs");
    esp_err_t e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool net_has_credentials(void) { return s_ssid[0] != 0; }
const char *net_ssid(void) { return s_ssid; }

void net_forget_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
    s_ssid[0] = s_pass[0] = 0;
}

/* ---------- station ---------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        if (s_retry < MAX_RETRY) {
            s_retry++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

esp_err_t net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_ev = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    load_creds();
    s_inited = true;
    return ESP_OK;
}

static bool sta_try(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    /* Drop any current link without letting the event handler reconnect,
     * and let its DISCONNECTED event pass before clearing the bits. */
    s_retry = MAX_RETRY;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        return false;
    }
    s_retry = 0;
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (!(b & BIT_GOT_IP)) {
        s_retry = MAX_RETRY; /* stop reconnect attempts */
        esp_wifi_disconnect();
        return false;
    }
    return true;
}

bool net_connect(uint32_t timeout_ms)
{
    if (!s_inited || !net_has_credentials()) {
        return false;
    }
    if (net_is_connected()) {
        return true;
    }
    if (!s_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_RETURN_ON_FALSE(esp_wifi_start() == ESP_OK, false, TAG, "start");
        s_started = true;
    }
    ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    bool ok = sta_try(s_ssid, s_pass, timeout_ms);
    ESP_LOGI(TAG, "%s", ok ? "connected" : "connect failed");
    return ok;
}

bool net_is_connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & BIT_GOT_IP);
}

void net_shutdown(void)
{
    if (s_inited) {
        net_portal_stop();
        s_retry = MAX_RETRY;
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = false;
    }
}

/* ---------- captive DNS: answer every A query with the AP address ---------- */

static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0 || bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns socket");
        goto out;
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const uint8_t ip[4] = {192, 168, 4, 1};
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            continue;
        }
        /* Find the end of the first question (QNAME + QTYPE + QCLASS). */
        int p = 12;
        while (p < n && buf[p]) {
            p += buf[p] + 1;
        }
        p += 5;
        if (p > n) {
            continue;
        }
        buf[2] = 0x81; buf[3] = 0x80;          /* response, recursion available */
        buf[6] = 0; buf[7] = 1;                /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4,
                               ip[0], ip[1], ip[2], ip[3]};
        memcpy(buf + p, ans, sizeof(ans));
        sendto(s_dns_sock, buf, p + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
out:
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ---------- portal HTTP ---------- */

static void html_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        const char *r = NULL;
        switch (*in) {
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '&': r = "&amp;"; break;
        case '"': r = "&quot;"; break;
        default: out[o++] = *in; continue;
        }
        o += snprintf(out + o, cap - o, "%s", r);
    }
    out[o] = 0;
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Basecamp setup</title><style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:28em;margin:auto;padding:1em}"
    "h1{font-size:1.3em}label{display:block;margin:.8em 0 .3em}"
    "select,input,button{width:100%;box-sizing:border-box;padding:.7em;font-size:1em;border-radius:.4em;border:1px solid #444;background:#222;color:#eee}"
    "button{background:#ff7a1a;color:#000;border:0;margin-top:1.2em;font-weight:600}"
    "p.s{color:#aaa;font-size:.9em}</style></head><body>";

static esp_err_t send_form(httpd_req_t *req, const char *notice)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Basecamp Wi-Fi setup</h1>");
    if (notice) {
        httpd_resp_sendstr_chunk(req, "<p style='color:#f66'>");
        httpd_resp_sendstr_chunk(req, notice);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req,
        "<p class=s>The launcher uses Wi-Fi to download the apps from GitHub. "
        "Pick your network (2.4 GHz) and enter its password.</p>"
        "<form method=post action=/save><label>Network</label><select name=ssid>");
    char esc[100], line[280];
    for (int i = 0; i < s_ap_count; i++) {
        const char *name = (const char *)s_aps[i].ssid;
        if (!name[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            dup |= strcmp(name, (const char *)s_aps[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        html_escape(esc, sizeof(esc), name);
        snprintf(line, sizeof(line), "<option value=\"%s\">%s (%d dBm%s)</option>", esc, esc,
                 s_aps[i].rssi, s_aps[i].authmode == WIFI_AUTH_OPEN ? ", open" : "");
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req,
        "</select><label>...or type a network name</label><input name=ssid2 maxlength=32 "
        "autocapitalize=none autocorrect=off placeholder='(optional)'>"
        "<label>Password</label><input name=pass type=password maxlength=64>"
        "<button type=submit>Connect</button></form>"
        "<p class=s><a style='color:#aaa' href=/?rescan=1>Scan again</a></p></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void scan_networks(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    s_ap_count = MAX_APS;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        esp_wifi_scan_get_ap_records(&s_ap_count, s_aps);
    } else {
        s_ap_count = 0;
    }
    ESP_LOGI(TAG, "scan: %u networks", s_ap_count);
}

static esp_err_t h_root(httpd_req_t *req)
{
    char q[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "rescan")) {
        scan_networks();
    }
    return send_form(req, NULL);
}

/* Decode one application/x-www-form-urlencoded field. */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char hx[3] = {p[1], p[2], 0};
                    out[o++] = (char)strtol(hx, NULL, 16);
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = 0;
    return false;
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[400];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[33], ssid2[33], pass[65];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "ssid2", ssid2, sizeof(ssid2));
    form_field(body, "pass", pass, sizeof(pass));
    if (ssid2[0]) {
        strlcpy(ssid, ssid2, sizeof(ssid));
    }
    if (!ssid[0]) {
        return send_form(req, "Choose or type a network name.");
    }
    if (pass[0] && strlen(pass) < 8) {
        return send_form(req, "Wi-Fi passwords are at least 8 characters.");
    }

    ESP_LOGI(TAG, "portal: trying '%s'", ssid);
    if (!sta_try(ssid, pass, 20000)) {
        char msg[180], esc[100];
        html_escape(esc, sizeof(esc), ssid);
        snprintf(msg, sizeof(msg), "Could not connect to \"%.99s\". Check the password and try again.", esc);
        return send_form(req, msg);
    }
    save_creds(ssid, pass);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Connected</h1><p>Basecamp is online and is now "
                                  "downloading the apps. You can close this page and "
                                  "rejoin your normal Wi-Fi.</p></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    xEventGroupSetBits(s_ev, BIT_PORTAL_OK);
    return ESP_OK;
}

/* Anything else (OS captive-portal probes included) -> redirect to the form. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" NET_PORTAL_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t net_portal_start(void)
{
    ESP_RETURN_ON_ERROR(net_init(), TAG, "init");
    if (s_portal) {
        return ESP_OK;
    }
    xEventGroupClearBits(s_ev, BIT_PORTAL_OK);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, NET_PORTAL_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(NET_PORTAL_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    s_retry = MAX_RETRY;
    esp_wifi_stop();
    s_started = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_started = true;
    scan_networks();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.lru_purge_enable = true;
    hc.max_open_sockets = 5;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "httpd");
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = h_root};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = h_save};
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_404);

    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);
    s_portal = true;
    ESP_LOGI(TAG, "portal up: join '%s', open http://%s", NET_PORTAL_SSID, NET_PORTAL_IP);
    return ESP_OK;
}

bool net_portal_wait(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_PORTAL_OK, pdFALSE, pdFALSE,
                                        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return b & BIT_PORTAL_OK;
}

void net_portal_stop(void)
{
    if (!s_portal) {
        return;
    }
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal = false;
}
