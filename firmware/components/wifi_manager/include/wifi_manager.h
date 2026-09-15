#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_MANAGER_UNCONFIGURED,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_RECONNECTING,
    WIFI_MANAGER_ERROR,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_state_t state;
    int8_t rssi;
    char ip[16];
    bool scanning;
} wifi_manager_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
} wifi_manager_network_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_reconfigure(void);
esp_err_t wifi_manager_scan_start(void);
size_t wifi_manager_get_networks(wifi_manager_network_t *networks, size_t capacity);
void wifi_manager_get_status(wifi_manager_status_t *status);
const char *wifi_manager_state_name(wifi_manager_state_t state);
