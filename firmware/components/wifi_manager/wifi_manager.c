#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "settings.h"

static wifi_manager_status_t status = { .state = WIFI_MANAGER_UNCONFIGURED };
static esp_netif_t *station_netif;
static bool started;
static bool configured;
#define WIFI_MANAGER_MAX_NETWORKS 8
static wifi_manager_network_t networks[WIFI_MANAGER_MAX_NETWORKS];
static size_t network_count;

static void bump_generation(void)
{
    ++status.generation;
}

static void update_ip(void)
{
    esp_netif_ip_info_t ip_info;
    if (station_netif != NULL && esp_netif_get_ip_info(station_netif, &ip_info) == ESP_OK) {
        inet_ntoa_r(ip_info.ip.addr, status.ip, sizeof(status.ip));
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!configured) return;
        status.state = WIFI_MANAGER_CONNECTING;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!configured) return;
        status.state = WIFI_MANAGER_RECONNECTING;
        status.ip[0] = '\0';
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_ap_record_t records[WIFI_MANAGER_MAX_NETWORKS];
        uint16_t found = WIFI_MANAGER_MAX_NETWORKS;
        network_count = 0;
        status.scanning = false;
        if (esp_wifi_scan_get_ap_records(&found, records) == ESP_OK) {
            for (uint16_t i = 0; i < found; ++i) {
                if (records[i].ssid[0] == '\0') continue;
                strlcpy(networks[network_count].ssid, (char *)records[i].ssid,
                        sizeof(networks[network_count].ssid));
                networks[network_count++].rssi = records[i].rssi;
            }
        }
        bump_generation();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* Moonraker consumes this state from another task.  Publish the
         * address before CONNECTED so it cannot open a TCP socket during the
         * short interval in which lwIP has no usable local IPv4 address. */
        update_ip();
        status.state = status.ip[0] != '\0' ? WIFI_MANAGER_CONNECTED : WIFI_MANAGER_CONNECTING;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) status.rssi = ap.rssi;
    }
}

static esp_err_t apply_settings(void)
{
    settings_wifi_t saved;
    ESP_RETURN_ON_ERROR(settings_get_wifi(&saved), "wifi_manager", "read Wi-Fi settings");
    configured = settings_wifi_is_configured(&saved);
    if (!configured) {
        status.state = WIFI_MANAGER_UNCONFIGURED;
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, saved.ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, saved.password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

esp_err_t wifi_manager_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), "wifi_manager", "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "wifi_manager", "event loop init");
    station_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), "wifi_manager", "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "wifi_manager", "Wi-Fi RAM storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL), "wifi_manager", "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL), "wifi_manager", "IP events");
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t result = apply_settings();
    started = true;
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    return esp_wifi_start();
}

esp_err_t wifi_manager_reconfigure(void)
{
    esp_err_t result = apply_settings();
    if (result != ESP_OK) return result;
    if (!started) return wifi_manager_start();
    status.state = WIFI_MANAGER_CONNECTING;
    return esp_wifi_connect();
}

void wifi_manager_get_status(wifi_manager_status_t *result)
{
    if (result != NULL) *result = status;
}

esp_err_t wifi_manager_scan_start(void)
{
    if (!started) ESP_RETURN_ON_ERROR(wifi_manager_start(), "wifi_manager", "start Wi-Fi for scan");
    if (status.scanning) return ESP_ERR_INVALID_STATE;
    network_count = 0;
    status.scanning = true;
    bump_generation();
    esp_err_t result = esp_wifi_scan_start(NULL, false);
    if (result != ESP_OK) {
        status.scanning = false;
        bump_generation();
    }
    return result;
}

size_t wifi_manager_get_networks(wifi_manager_network_t *result, size_t capacity)
{
    if (result == NULL || capacity == 0) return network_count;
    size_t count = network_count < capacity ? network_count : capacity;
    memcpy(result, networks, count * sizeof(*result));
    return count;
}

const char *wifi_manager_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_UNCONFIGURED: return "not configured";
    case WIFI_MANAGER_CONNECTING: return "connecting";
    case WIFI_MANAGER_CONNECTED: return "connected";
    case WIFI_MANAGER_RECONNECTING: return "reconnecting";
    default: return "error";
    }
}
