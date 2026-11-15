#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "netmgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

#define NVS_NS       "basecamp"
#define BIT_GOT_IP   BIT0
#define BIT_FAILED   BIT1
#define BIT_CREDS    BIT2
#define MAX_RETRY    12     /* quick retries per attempt; the caller's timeout bounds the total */
#define MAX_APS      20

static EventGroupHandle_t s_ev;
static bool s_inited, s_ap_up, s_started;
static volatile net_sta_t s_sta = NET_STA_NONE;
static volatile int s_retry;
static char s_ssid[33], s_pass[65];

static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static int s_dns_sock = -1;

static wifi_ap_record_t s_aps[MAX_APS];
static uint16_t s_ap_count;
static volatile bool s_scanning;
static volatile int s_reason;               /* last station disconnect reason (0 = none) */

/* ---------- credentials ---------- */

static void load_creds(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t l = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &l) != ESP_OK) s_ssid[0] = 0;
    l = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &l) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
}

bool net_has_credentials(void) { return s_ssid[0] != 0; }
const char *net_ssid(void) { return s_ssid; }

esp_err_t net_set_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs");
    esp_err_t e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    /* A changed network: drop the old link so the next net_sta_connect() uses the new one. */
    if (s_inited && s_sta != NET_STA_NONE) {
        s_sta = NET_STA_NONE;
        s_retry = MAX_RETRY;
        esp_wifi_disconnect();
    }
    xEventGroupSetBits(s_ev, BIT_CREDS);
    return ESP_OK;
}

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
    if (s_inited) {
        s_sta = NET_STA_NONE;
        s_retry = MAX_RETRY;
        esp_wifi_disconnect();
    }
}

bool net_wait_creds(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CREDS, pdTRUE, pdFALSE,
                                        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return b & BIT_CREDS;
}

/* ---------- events ---------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta == NET_STA_NONE || s_sta == NET_STA_FAILED) {
            return;                                   /* deliberate disconnect */
        }
        s_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        ESP_LOGW(TAG, "station disconnected, reason %d (retry %d)", s_reason, s_retry);
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        if (s_retry < MAX_RETRY) {
            s_retry++;
            s_sta = NET_STA_CONNECTING;
            esp_wifi_connect();
        } else {
            s_sta = NET_STA_FAILED;
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        s_sta = NET_STA_CONNECTED;
        xEventGroupClearBits(s_ev, BIT_FAILED);
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_ap_count = MAX_APS;
        if (esp_wifi_scan_get_ap_records(&s_ap_count, s_aps) != ESP_OK) {
            s_ap_count = 0;
        }
        s_scanning = false;
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
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    load_creds();
    s_inited = true;
    return ESP_OK;
}

/* ---------- station ---------- */

void net_sta_connect(void)
{
    if (!s_started || !net_has_credentials() || s_sta == NET_STA_CONNECTED || s_sta == NET_STA_CONNECTING) {
        return;
    }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        s_sta = NET_STA_FAILED;
        xEventGroupSetBits(s_ev, BIT_FAILED);
        return;
    }
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
    s_retry = 0;
    s_reason = 0;
    s_sta = NET_STA_CONNECTING;
    ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    if (esp_wifi_connect() != ESP_OK) {
        s_reason = 0xFFFF;
        s_sta = NET_STA_FAILED;
        xEventGroupSetBits(s_ev, BIT_FAILED);
    }
}

bool net_sta_wait(uint32_t timeout_ms)
{
    if (s_sta == NET_STA_CONNECTED) {
        return true;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(b & BIT_GOT_IP) && s_sta == NET_STA_CONNECTING) {   /* timed out: stop retrying */
        s_sta = NET_STA_FAILED;
        s_retry = MAX_RETRY;
        esp_wifi_disconnect();
    }
    return b & BIT_GOT_IP;
}

net_sta_t net_sta_state(void) { return s_sta; }

const char *net_fail_text(void)
{
    static char buf[24];
    switch (s_reason) {
    case 0:
        return "";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password?";
    case 0xFFFF:
        return "radio not ready";
    default:
        snprintf(buf, sizeof(buf), "error %d", s_reason);
        return buf;
    }
}

/* ---------- scan ---------- */

esp_err_t net_scan_start(void)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_OK;
    wifi_scan_config_t sc = {.show_hidden = false};
    s_scanning = true;
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e != ESP_OK) s_scanning = false;
    return e;
}

bool net_scan_running(void) { return s_scanning; }

int net_scan_results(net_ap_info_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_ap_count && n < max; i++) {       /* records arrive strongest first */
        const char *name = (const char *)s_aps[i].ssid;
        if (!name[0]) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) dup |= strcmp(out[j].ssid, name) == 0;
        if (dup) continue;
        strlcpy(out[n].ssid, name, sizeof(out[n].ssid));
        out[n].rssi = s_aps[i].rssi;
        out[n].open = s_aps[i].authmode == WIFI_AUTH_OPEN;
        n++;
    }
    return n;
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
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            continue;
        }
        int p = 12;                                          /* end of the first question */
        while (p < n && buf[p]) {
            p += buf[p] + 1;
        }
        p += 5;
        if (p > n) {
            continue;
        }
        buf[2] = 0x81; buf[3] = 0x80;                        /* response, recursion available */
        buf[6] = 0; buf[7] = 1;                              /* one answer */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
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

/* ---------- access point ---------- */

static esp_err_t radio_start(void)
{
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_started = true;
        vTaskDelay(pdMS_TO_TICKS(300));            /* let the driver finish starting before the first connect */
    }
    return ESP_OK;
}

esp_err_t net_sta_start(void)
{
    ESP_RETURN_ON_ERROR(net_init(), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_ap_up ? WIFI_MODE_APSTA : WIFI_MODE_STA), TAG, "mode");
    return radio_start();
}

esp_err_t net_ap_start(void)
{
    ESP_RETURN_ON_ERROR(net_init(), TAG, "init");
    if (s_ap_up) {
        return ESP_OK;
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, NET_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(NET_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;                         /* open by design: no password */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");
    ESP_RETURN_ON_ERROR(radio_start(), TAG, "start");
    s_ap_up = true;
    s_dns_run = true;
    if (!s_dns_task) {
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);
    }
    ESP_LOGI(TAG, "open network '%s' up, web page at http://%s", NET_AP_SSID, NET_AP_IP);
    return ESP_OK;
}

void net_ap_stop(void)
{
    if (!s_ap_up) {
        return;
    }
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_wifi_set_mode(WIFI_MODE_STA);                        /* the station link stays up */
    s_ap_up = false;
    ESP_LOGI(TAG, "setup network off");
}

bool net_ap_active(void) { return s_ap_up; }

bool net_sta_ip(char out[16])
{
    out[0] = 0;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    snprintf(out, 16, IPSTR, IP2STR(&ip.ip));
    return true;
}

void net_shutdown(void)
{
    if (!s_inited) {
        return;
    }
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_sta = NET_STA_NONE;
    s_retry = MAX_RETRY;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_ap_up = false;
    s_started = false;
}
