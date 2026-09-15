#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_WIFI_SSID_MAX_LEN 32
#define SETTINGS_WIFI_PASSWORD_MAX_LEN 64
#define SETTINGS_PRINTER_NAME_MAX_LEN 32
#define SETTINGS_PRINTER_HOST_MAX_LEN 63
#define SETTINGS_MAX_PRINTERS 16

typedef struct {
    char ssid[SETTINGS_WIFI_SSID_MAX_LEN + 1];
    char password[SETTINGS_WIFI_PASSWORD_MAX_LEN + 1];
} settings_wifi_t;

typedef struct {
    char name[SETTINGS_PRINTER_NAME_MAX_LEN + 1];
    char host[SETTINGS_PRINTER_HOST_MAX_LEN + 1];
    uint16_t port;
    bool enabled;
} settings_printer_t;

typedef enum {
    SETTINGS_THEME_LIGHT,
    SETTINGS_THEME_DARK,
} settings_theme_t;

esp_err_t settings_init(void);
esp_err_t settings_get_wifi(settings_wifi_t *wifi);
esp_err_t settings_set_wifi(const settings_wifi_t *wifi);
esp_err_t settings_get_printer(settings_printer_t *printer);
esp_err_t settings_set_printer(const settings_printer_t *printer);
esp_err_t settings_get_printers(settings_printer_t *printers, size_t capacity, size_t *count);
esp_err_t settings_get_printer_at(size_t index, settings_printer_t *printer);
esp_err_t settings_set_printer_at(size_t index, const settings_printer_t *printer);
esp_err_t settings_add_printer(const settings_printer_t *printer, size_t *index);
esp_err_t settings_get_active_printer_index(size_t *index);
esp_err_t settings_set_active_printer_index(size_t index);
esp_err_t settings_get_theme(settings_theme_t *theme);
esp_err_t settings_set_theme(settings_theme_t theme);

bool settings_wifi_is_configured(const settings_wifi_t *wifi);
bool settings_printer_is_valid(const settings_printer_t *printer);
