#ifndef ESP_WIFI_BSP_H
#define ESP_WIFI_BSP_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize WiFi in STA mode and start connecting to the given network.
// Call once at startup. Auto-reconnects on disconnect.
void espwifi_Init(const char *ssid, const char *password);

// Block until connected (got IP) or timeout_ms elapses (0 = wait forever).
// Returns true if connected.
bool espwifi_wait_connected(uint32_t timeout_ms);

// True if currently connected (has an IP lease).
bool espwifi_is_connected(void);

// Copy the current STA IPv4 (e.g. "10.0.0.31") into out. Returns false if no IP.
bool espwifi_get_ip(char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
