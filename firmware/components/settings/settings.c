#include "settings.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define SETTINGS_NAMESPACE "pendant"
#define SETTINGS_WIFI_KEY "wifi"
#define SETTINGS_PRINTER_KEY "printer"
#define SETTINGS_PRINTERS_KEY "printers"
#define SETTINGS_THEME_KEY "theme"

typedef struct {
    uint8_t count;
    uint8_t active_index;
    settings_printer_t items[SETTINGS_MAX_PRINTERS];
} settings_printer_store_t;

/* Format written by firmware versions that supported four printers. */
typedef struct {
    uint8_t count;
    uint8_t active_index;
    settings_printer_t items[4];
} settings_printer_store_v1_t;

/* The printer fleet is one NVS blob.  In particular, selecting a printer
 * updates active_index by reading and writing that entire blob.  Selection is
 * performed from a worker task, while adding/editing is performed by the UI
 * task, so those read-modify-write operations must not overlap. */
static SemaphoreHandle_t settings_mutex;
/* A 16-printer fleet is about 1.6 KiB. Keeping a copy on each caller's
 * stack caused the Moonraker task to overflow while it was polling the
 * active printer at startup. All access is already serialized by
 * settings_mutex, so one shared scratch buffer is safe. */
static settings_printer_store_t printer_store_scratch;
static settings_printer_store_v1_t printer_store_v1_scratch;

static esp_err_t lock_settings(void)
{
    if (settings_mutex == NULL) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(settings_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void unlock_settings(void)
{
    xSemaphoreGive(settings_mutex);
}

static esp_err_t get_blob(const char *key, void *value, size_t size)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    size_t stored_size = size;
    result = nvs_get_blob(handle, key, value, &stored_size);
    nvs_close(handle);
    return result == ESP_OK && stored_size == size ? ESP_OK :
           (result == ESP_OK ? ESP_ERR_INVALID_SIZE : result);
}

static esp_err_t set_blob(const char *key, const void *value, size_t size)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_blob(handle, key, value, size);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return result;
}

esp_err_t settings_init(void)
{
    if (settings_mutex == NULL) settings_mutex = xSemaphoreCreateMutex();
    return settings_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t settings_get_theme(settings_theme_t *theme)
{
    if (theme == NULL) return ESP_ERR_INVALID_ARG;
    *theme = SETTINGS_THEME_LIGHT;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    uint8_t value = SETTINGS_THEME_LIGHT;
    result = nvs_get_u8(handle, SETTINGS_THEME_KEY, &value);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK || value > SETTINGS_THEME_DARK) return ESP_ERR_INVALID_ARG;
    *theme = (settings_theme_t)value;
    return ESP_OK;
}

esp_err_t settings_set_theme(settings_theme_t theme)
{
    if (theme > SETTINGS_THEME_DARK) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_u8(handle, SETTINGS_THEME_KEY, (uint8_t)theme);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (result == ESP_OK || handle) nvs_close(handle);
    return result;
}

esp_err_t settings_get_wifi(settings_wifi_t *wifi)
{
    if (wifi == NULL) return ESP_ERR_INVALID_ARG;
    memset(wifi, 0, sizeof(*wifi));
    esp_err_t result = get_blob(SETTINGS_WIFI_KEY, wifi, sizeof(*wifi));
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

esp_err_t settings_set_wifi(const settings_wifi_t *wifi)
{
    if (wifi == NULL || !settings_wifi_is_configured(wifi)) return ESP_ERR_INVALID_ARG;
    if (strnlen(wifi->ssid, sizeof(wifi->ssid)) == sizeof(wifi->ssid) ||
        strnlen(wifi->password, sizeof(wifi->password)) == sizeof(wifi->password)) return ESP_ERR_INVALID_SIZE;
    return set_blob(SETTINGS_WIFI_KEY, wifi, sizeof(*wifi));
}

esp_err_t settings_get_printer(settings_printer_t *printer)
{
    if (printer == NULL) return ESP_ERR_INVALID_ARG;
    memset(printer, 0, sizeof(*printer));
    printer->port = 7125;
    size_t index = 0;
    ESP_RETURN_ON_ERROR(settings_get_active_printer_index(&index), "settings", "read active printer");
    esp_err_t result = settings_get_printer_at(index, printer);
    return result == ESP_ERR_NOT_FOUND ? ESP_OK : result;
}

esp_err_t settings_set_printer(const settings_printer_t *printer)
{
    size_t index = 0;
    ESP_RETURN_ON_ERROR(settings_get_active_printer_index(&index), "settings", "read active printer");
    settings_printer_t current;
    if (settings_get_printer_at(index, &current) != ESP_OK || !settings_printer_is_valid(&current)) {
        return settings_add_printer(printer, NULL);
    }
    return settings_set_printer_at(index, printer);
}

static esp_err_t get_printer_store(settings_printer_store_t *store)
{
    if (store == NULL) return ESP_ERR_INVALID_ARG;
    memset(store, 0, sizeof(*store));
    esp_err_t result = get_blob(SETTINGS_PRINTERS_KEY, store, sizeof(*store));
    if (result == ESP_ERR_INVALID_SIZE) {
        result = get_blob(SETTINGS_PRINTERS_KEY, &printer_store_v1_scratch,
                          sizeof(printer_store_v1_scratch));
        if (result == ESP_OK && printer_store_v1_scratch.count <= 4) {
            store->count = printer_store_v1_scratch.count;
            store->active_index = printer_store_v1_scratch.active_index < printer_store_v1_scratch.count ?
                                  printer_store_v1_scratch.active_index : 0;
            memcpy(store->items, printer_store_v1_scratch.items,
                   printer_store_v1_scratch.count * sizeof(printer_store_v1_scratch.items[0]));
            return ESP_OK;
        }
    }
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        settings_printer_t legacy = { .port = 7125 };
        result = get_blob(SETTINGS_PRINTER_KEY, &legacy, sizeof(legacy));
        if (result == ESP_OK && settings_printer_is_valid(&legacy)) {
            store->items[0] = legacy;
            store->count = 1;
        } else if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
            return result;
        }
        return ESP_OK;
    }
    if (result != ESP_OK) return result;
    if (store->count > SETTINGS_MAX_PRINTERS) return ESP_ERR_INVALID_SIZE;
    if (store->count == 0) store->active_index = 0;
    else if (store->active_index >= store->count) store->active_index = 0;
    return ESP_OK;
}

static esp_err_t set_printer_store(const settings_printer_store_t *store)
{
    return set_blob(SETTINGS_PRINTERS_KEY, store, sizeof(*store));
}

esp_err_t settings_get_printers(settings_printer_t *printers, size_t capacity, size_t *count)
{
    ESP_RETURN_ON_ERROR(lock_settings(), "settings", "lock printers");
    esp_err_t result = get_printer_store(&printer_store_scratch);
    if (result != ESP_OK) {
        unlock_settings();
        return result;
    }
    if (count != NULL) *count = printer_store_scratch.count;
    if (printers != NULL && capacity > 0) {
        size_t copied = printer_store_scratch.count < capacity ? printer_store_scratch.count : capacity;
        memcpy(printers, printer_store_scratch.items, copied * sizeof(*printers));
    }
    result = printer_store_scratch.count > capacity && printers != NULL ? ESP_ERR_INVALID_SIZE : ESP_OK;
    unlock_settings();
    return result;
}

esp_err_t settings_get_printer_at(size_t index, settings_printer_t *printer)
{
    if (printer == NULL) return ESP_ERR_INVALID_ARG;
    memset(printer, 0, sizeof(*printer));
    printer->port = 7125;
    ESP_RETURN_ON_ERROR(lock_settings(), "settings", "lock printers");
    esp_err_t result = get_printer_store(&printer_store_scratch);
    if (result == ESP_OK && index >= printer_store_scratch.count) result = ESP_ERR_NOT_FOUND;
    if (result != ESP_OK) {
        unlock_settings();
        return result;
    }
    *printer = printer_store_scratch.items[index];
    unlock_settings();
    return ESP_OK;
}

esp_err_t settings_set_printer_at(size_t index, const settings_printer_t *printer)
{
    if (!settings_printer_is_valid(printer)) return ESP_ERR_INVALID_ARG;
    if (strnlen(printer->name, sizeof(printer->name)) == sizeof(printer->name) ||
        strnlen(printer->host, sizeof(printer->host)) == sizeof(printer->host)) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(lock_settings(), "settings", "lock printers");
    esp_err_t result = get_printer_store(&printer_store_scratch);
    if (result == ESP_OK && index >= printer_store_scratch.count) result = ESP_ERR_NOT_FOUND;
    if (result != ESP_OK) {
        unlock_settings();
        return result;
    }
    printer_store_scratch.items[index] = *printer;
    result = set_printer_store(&printer_store_scratch);
    unlock_settings();
    return result;
}

esp_err_t settings_add_printer(const settings_printer_t *printer, size_t *index)
{
    if (!settings_printer_is_valid(printer)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_settings(), "settings", "lock printers");
    esp_err_t result = get_printer_store(&printer_store_scratch);
    if (result == ESP_OK && printer_store_scratch.count >= SETTINGS_MAX_PRINTERS) result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        unlock_settings();
        return result;
    }
    printer_store_scratch.items[printer_store_scratch.count] = *printer;
    if (index != NULL) *index = printer_store_scratch.count;
    printer_store_scratch.count++;
    result = set_printer_store(&printer_store_scratch);
    unlock_settings();
    return result;
}

esp_err_t settings_get_active_printer_index(size_t *index)
{
    if (index == NULL) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_settings(), "settings", "lock printers");
    esp_err_t result = get_printer_store(&printer_store_scratch);
    if (result != ESP_OK) {
        unlock_settings();
        return result;
    }
    *index = printer_store_scratch.active_index;
    unlock_settings();
    return ESP_OK;
}

esp_err_t settings_set_active_printer_index(size_t index)
{
    ESP_RETURN_ON_ERROR(lock_settings(), "settings", "lock printers");
    esp_err_t result = get_printer_store(&printer_store_scratch);
    if (result == ESP_OK && index >= printer_store_scratch.count) result = ESP_ERR_NOT_FOUND;
    if (result != ESP_OK) {
        unlock_settings();
        return result;
    }
    printer_store_scratch.active_index = (uint8_t)index;
    result = set_printer_store(&printer_store_scratch);
    unlock_settings();
    return result;
}

bool settings_wifi_is_configured(const settings_wifi_t *wifi)
{
    return wifi != NULL && wifi->ssid[0] != '\0';
}

bool settings_printer_is_valid(const settings_printer_t *printer)
{
    return printer != NULL && printer->name[0] != '\0' && printer->host[0] != '\0' &&
           printer->port != 0;
}
