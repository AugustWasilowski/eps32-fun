#include <stdio.h>
#include <string.h>
#include "esp_wifi_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_eg = NULL;
#define WIFI_CONNECTED_BIT BIT0
static char s_ip_str[16] = "";  // current STA IPv4, "" if not connected

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected; reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

void espwifi_Init(const char *ssid, const char *password)
{
    s_wifi_eg = xEventGroupCreate();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    // Battery saver: stay associated but let the radio sleep across several DTIM
    // beacons. Cuts idle draw roughly in half vs the default; the only cost is that
    // pushed replies/TTS arrive a few hundred ms later, which is fine for a desk buddy.
    wifi_config.sta.listen_interval = 3;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));   // honor listen_interval above
    ESP_LOGI(TAG, "connecting to SSID '%s'...", ssid);
}

bool espwifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_eg) return false;
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool espwifi_is_connected(void)
{
    return s_wifi_eg && (xEventGroupGetBits(s_wifi_eg) & WIFI_CONNECTED_BIT);
}

bool espwifi_get_ip(char *out, size_t len)
{
    if (!out || len == 0 || s_ip_str[0] == '\0') return false;
    strncpy(out, s_ip_str, len - 1);
    out[len - 1] = '\0';
    return true;
}
