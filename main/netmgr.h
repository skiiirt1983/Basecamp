#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Launcher Wi-Fi.
 *  - Access point "BaseCamp-Setup" (open, no password) is up while the menu is
 *    shown. A catch-all DNS makes phones open the web page by themselves.
 *  - The station side (your router, for GitHub downloads) connects only on
 *    demand. Credentials live in NVS namespace "basecamp" (ssid, pass).
 */
#define NET_AP_SSID "BaseCamp-Setup"
#define NET_AP_IP   "192.168.4.1"

esp_err_t net_init(void);
esp_err_t net_ap_start(void);
void net_shutdown(void);                    /* radio off, before handing over to an app */

bool net_has_credentials(void);
const char *net_ssid(void);
esp_err_t net_set_credentials(const char *ssid, const char *pass);
void net_forget_credentials(void);
/* Block until credentials are saved (true) or timeout. */
bool net_wait_creds(uint32_t timeout_ms);

typedef enum { NET_STA_NONE, NET_STA_CONNECTING, NET_STA_CONNECTED, NET_STA_FAILED } net_sta_t;
net_sta_t net_sta_state(void);
void net_sta_connect(void);                 /* async; no-op when connecting/connected/no credentials */
bool net_sta_wait(uint32_t timeout_ms);     /* true when connected */

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_ap_info_t;
esp_err_t net_scan_start(void);             /* async */
bool net_scan_running(void);
int net_scan_results(net_ap_info_t *out, int max);   /* strongest first, de-duplicated */
