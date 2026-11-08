#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Wi-Fi for the launcher: station mode with credentials stored in NVS
 * namespace "basecamp" (keys "ssid", "pass"), plus a captive setup portal
 * (SoftAP "Basecamp-Setup", catch-all DNS, HTTP form) when none are stored. */

#define NET_PORTAL_SSID "Basecamp-Setup"
#define NET_PORTAL_IP   "192.168.4.1"

esp_err_t net_init(void);
bool net_has_credentials(void);
void net_forget_credentials(void);

/* Connect with the stored credentials; true once an IP address is obtained. */
bool net_connect(uint32_t timeout_ms);
bool net_is_connected(void);
/* SSID currently stored (empty string if none). */
const char *net_ssid(void);

/* Captive portal. net_portal_start() returns immediately; the portal saves
 * credentials only after they produced a working connection. */
esp_err_t net_portal_start(void);
/* Blocks until the portal produced a working connection (true) or timeout. */
bool net_portal_wait(uint32_t timeout_ms);
void net_portal_stop(void);

/* Switch the radio off before handing over to an app. */
void net_shutdown(void);
