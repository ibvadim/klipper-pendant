#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "moonraker.h"

#define OTA_TAGS_API_URL "https://api.github.com/repos/ibvadim/klipper-pendant/tags?per_page=8"
#define OTA_LATEST_MANIFEST_URL "https://github.com/ibvadim/klipper-pendant/releases/latest/download/manifest.json"
#define OTA_BOARD "onx3248g035"
#define OTA_URL_PREFIX "https://github.com/ibvadim/klipper-pendant/releases/download/"
#define OTA_MANIFEST_MAX_SIZE 2048
/* Tags contain only names and commit ids, unlike the Releases API which also
 * embeds arbitrary-length release notes.  Manifests remain the authority for
 * whether a tag is an installable release. */
#define OTA_RELEASE_INDEX_MAX_SIZE 4096
#define OTA_HTTP_BUFFER_SIZE 2048
#define OTA_CHECK_TASK_STACK_SIZE 8192
#define OTA_INSTALL_TASK_STACK_SIZE 8192

typedef struct {
    char version[OTA_UPDATE_VERSION_MAX_LEN];
    char url[256];
    uint8_t sha256[32];
} ota_release_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} manifest_response_t;

typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    mbedtls_sha256_context sha;
    esp_err_t result;
    int content_length;
    int downloaded;
    bool started;
} download_response_t;

static const char *TAG = "ota_update";
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_update_status_t status;
static ota_release_t pending_release;
/* The release catalogue is only useful after a check, so keep it in PSRAM
 * instead of permanently consuming the internal RAM required at boot. */
static ota_release_t *discovered_releases;

/*
 * OTA writes flash, so its stack must remain in internal RAM.  Reserve one
 * stack at boot and reuse it for both mutually-exclusive OTA operations. This
 * avoids a failed large heap allocation after the UI and network services have
 * fragmented internal RAM.
 */
static StaticTask_t ota_task_buffer;
static StackType_t ota_task_stack[OTA_INSTALL_TASK_STACK_SIZE];

static void set_status(ota_update_state_t state, const char *message, uint8_t progress);
static esp_err_t get_http_response(const char *url, char *buffer, size_t buffer_size,
                                   bool github_api);
static bool parse_manifest(const char *text, ota_release_t *release);

static BaseType_t create_ota_task(TaskFunction_t function, const char *name,
                                  configSTACK_DEPTH_TYPE stack_size)
{
    ESP_LOGI(TAG, "Creating %s task: internal free=%u, largest=%u",
             name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return xTaskCreateStatic(function, name, stack_size, NULL, 4,
                             ota_task_stack, &ota_task_buffer) == NULL ? pdFAIL : pdPASS;
}

static esp_err_t manifest_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == NULL) return ESP_OK;

    manifest_response_t *response = event->user_data;
    if ((size_t)event->data_len > response->capacity - 1 - response->length) {
        response->overflow = true;
        return ESP_OK;
    }
    memcpy(response->buffer + response->length, event->data, event->data_len);
    response->length += event->data_len;
    return ESP_OK;
}

static esp_err_t download_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == NULL) return ESP_OK;

    download_response_t *response = event->user_data;
    if (response->result != ESP_OK) return response->result;
    if (esp_http_client_get_status_code(event->client) != 200) {
        response->result = ESP_FAIL;
        return response->result;
    }
    if (!response->started) {
        response->content_length = esp_http_client_get_content_length(event->client);
        if ((response->content_length > 0 && (size_t)response->content_length > response->partition->size) ||
            esp_ota_begin(response->partition, OTA_SIZE_UNKNOWN, &response->handle) != ESP_OK) {
            response->result = ESP_FAIL;
            return response->result;
        }
        mbedtls_sha256_init(&response->sha);
        if (mbedtls_sha256_starts(&response->sha, 0) != 0) {
            response->result = ESP_FAIL;
            return response->result;
        }
        response->started = true;
    }
    response->result = esp_ota_write(response->handle, event->data, event->data_len);
    if (response->result == ESP_OK &&
        mbedtls_sha256_update(&response->sha, event->data, event->data_len) != 0) {
        response->result = ESP_FAIL;
    }
    if (response->result == ESP_OK) {
        response->downloaded += event->data_len;
        if (response->content_length > 0) {
            set_status(OTA_UPDATE_DOWNLOADING, "Downloading firmware",
                       (uint8_t)((response->downloaded * 100LL) / response->content_length));
        }
    }
    return response->result;
}

static void set_status(ota_update_state_t state, const char *message, uint8_t progress)
{
    portENTER_CRITICAL(&status_lock);
    status.state = state;
    status.progress = progress;
    strlcpy(status.message, message == NULL ? "" : message, sizeof(status.message));
    portEXIT_CRITICAL(&status_lock);
}

void ota_update_get_status(ota_update_status_t *result)
{
    if (result == NULL) return;
    portENTER_CRITICAL(&status_lock);
    *result = status;
    strlcpy(result->current_version, esp_app_get_description()->version,
            sizeof(result->current_version));
    portEXIT_CRITICAL(&status_lock);
}

const char *ota_update_state_name(ota_update_state_t state)
{
    switch (state) {
    case OTA_UPDATE_CHECKING: return "Checking";
    case OTA_UPDATE_AVAILABLE: return "Ready";
    case OTA_UPDATE_UP_TO_DATE: return "Up to date";
    case OTA_UPDATE_DOWNLOADING: return "Downloading";
    case OTA_UPDATE_VERIFYING: return "Verifying";
    case OTA_UPDATE_RESTARTING: return "Restarting";
    case OTA_UPDATE_ERROR: return "Error";
    case OTA_UPDATE_IDLE:
    default: return "Not checked";
    }
}

static bool parse_sha256(const char *text, uint8_t output[32])
{
    if (text == NULL || strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        const char high = text[i * 2];
        const char low = text[i * 2 + 1];
        const int hi = high >= '0' && high <= '9' ? high - '0' :
                       high >= 'a' && high <= 'f' ? high - 'a' + 10 :
                       high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
        const int lo = low >= '0' && low <= '9' ? low - '0' :
                       low >= 'a' && low <= 'f' ? low - 'a' + 10 :
                       low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return false;
        output[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static int compare_versions(const char *left, const char *right)
{
    if (left == NULL || right == NULL) return 0;
    if (*left == 'v') ++left;
    if (*right == 'v') ++right;
    for (int i = 0; i < 3; ++i) {
        char *left_end;
        char *right_end;
        const unsigned long l = strtoul(left, &left_end, 10);
        const unsigned long r = strtoul(right, &right_end, 10);
        if (left_end == left || right_end == right) return strcmp(left, right);
        if (l != r) return l < r ? -1 : 1;
        left = *left_end == '.' ? left_end + 1 : left_end;
        right = *right_end == '.' ? right_end + 1 : right_end;
    }
    return strcmp(left, right);
}

static esp_err_t get_manifest(char *buffer, size_t buffer_size)
{
    return get_http_response(OTA_LATEST_MANIFEST_URL, buffer, buffer_size, false);
}

static esp_err_t get_http_response(const char *url, char *buffer, size_t buffer_size,
                                   bool github_api)
{
    if (buffer == NULL || buffer_size < 2) return ESP_ERR_INVALID_ARG;
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "TLS heap before manifest request: internal free=%u, largest=%u",
             (unsigned)free_internal, (unsigned)largest_internal);
    manifest_response_t response = {
        .buffer = buffer,
        .capacity = buffer_size,
    };
    buffer[0] = '\0';
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "klipper-pendant-ota/1.0",
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
        .event_handler = manifest_http_event,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    if (github_api) esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    if (result != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        ESP_LOGE(TAG, "Release request failed: result=%s, HTTP=%d, bytes=%u%s",
                 esp_err_to_name(result), status_code, (unsigned)response.length,
                 response.overflow ? ", response too large" : "");
        result = result == ESP_OK ? ESP_FAIL : result;
    } else {
        buffer[response.length] = '\0';
        ESP_LOGI(TAG, "Release response received: HTTP=%d, bytes=%u", status_code,
                 (unsigned)response.length);
    }
    esp_http_client_cleanup(client);
    return result;
}

static bool valid_tag_name(const char *tag)
{
    if (tag == NULL || tag[0] != 'v' || tag[1] == '\0' || strlen(tag) >= OTA_UPDATE_VERSION_MAX_LEN) return false;
    for (const char *cursor = tag; *cursor != '\0'; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '.' || *cursor == '-' || *cursor == '_')) {
            return false;
        }
    }
    return true;
}

static size_t get_release_tags(const char *text, char tags[OTA_UPDATE_MAX_RELEASES][OTA_UPDATE_VERSION_MAX_LEN])
{
    cJSON *root = cJSON_Parse(text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return 0;
    }
    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (count == OTA_UPDATE_MAX_RELEASES) break;
        if (!cJSON_IsString(tag) || !valid_tag_name(tag->valuestring)) continue;
        strlcpy(tags[count++], tag->valuestring, OTA_UPDATE_VERSION_MAX_LEN);
    }
    cJSON_Delete(root);
    return count;
}

static size_t get_published_releases(ota_release_t releases[OTA_UPDATE_MAX_RELEASES],
                                     char *release_index_buffer, size_t index_buffer_size,
                                     char *manifest, size_t manifest_size)
{
    char tags[OTA_UPDATE_MAX_RELEASES][OTA_UPDATE_VERSION_MAX_LEN] = { 0 };
    if (get_http_response(OTA_TAGS_API_URL, release_index_buffer,
                          index_buffer_size, true) != ESP_OK) return 0;
    const size_t tag_count = get_release_tags(release_index_buffer, tags);
    size_t count = 0;
    for (size_t i = 0; i < tag_count; ++i) {
        char manifest_url[192];
        snprintf(manifest_url, sizeof(manifest_url), "%s%s/manifest.json", OTA_URL_PREFIX, tags[i]);
        if (get_http_response(manifest_url, manifest, manifest_size, false) == ESP_OK &&
            parse_manifest(manifest, &releases[count])) {
            ++count;
        } else {
            ESP_LOGW(TAG, "Skipping release %s: its manifest is unavailable or invalid", tags[i]);
        }
    }
    return count;
}

static bool parse_manifest(const char *text, ota_release_t *release)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) return false;
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *board = cJSON_GetObjectItemCaseSensitive(root, "board");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const bool valid = cJSON_IsNumber(format) && format->valueint == 1 &&
                       cJSON_IsString(board) && strcmp(board->valuestring, OTA_BOARD) == 0 &&
                       cJSON_IsString(version) && strnlen(version->valuestring, sizeof(release->version)) < sizeof(release->version) &&
                       cJSON_IsString(url) && strncmp(url->valuestring, OTA_URL_PREFIX, strlen(OTA_URL_PREFIX)) == 0 &&
                       strnlen(url->valuestring, sizeof(release->url)) < sizeof(release->url) &&
                       cJSON_IsString(sha256) && parse_sha256(sha256->valuestring, release->sha256);
    if (valid) {
        strlcpy(release->version, version->valuestring, sizeof(release->version));
        strlcpy(release->url, url->valuestring, sizeof(release->url));
    }
    cJSON_Delete(root);
    return valid;
}

static void check_task(void *argument)
{
    (void)argument;
    /* Do not make a second TLS client compete with the live WebSocket.  The
     * UI asks for this on entry too; this wait makes the handover deterministic
     * when Check is pressed immediately after opening Firmware. */
    moonraker_set_suspended(true);
    for (uint8_t attempt = 0; attempt < 20 && !moonraker_is_suspended(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!moonraker_is_suspended()) {
        set_status(OTA_UPDATE_ERROR, "Could not pause printer service", 0);
        vTaskDelete(NULL);
        return;
    }
    ota_release_t fallback_release = { 0 };
    ota_release_t *releases = heap_caps_calloc(OTA_UPDATE_MAX_RELEASES, sizeof(*releases),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *release_index_buffer = heap_caps_malloc(OTA_RELEASE_INDEX_MAX_SIZE,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *manifest = heap_caps_malloc(OTA_MANIFEST_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t release_count = releases != NULL && release_index_buffer != NULL && manifest != NULL ?
        get_published_releases(releases, release_index_buffer, OTA_RELEASE_INDEX_MAX_SIZE,
                               manifest, OTA_MANIFEST_MAX_SIZE) : 0;
    free(release_index_buffer);
    /* Keep the old endpoint as a fallback for devices behind networks that
     * allow github.com but not api.github.com. */
    if (release_count == 0 && manifest != NULL &&
        get_manifest(manifest, OTA_MANIFEST_MAX_SIZE) == ESP_OK &&
        parse_manifest(manifest, &fallback_release)) {
        if (releases != NULL) releases[0] = fallback_release;
        release_count = 1;
    }
    if (release_count == 0) {
        free(releases);
        free(manifest);
        set_status(OTA_UPDATE_ERROR, "Release list unavailable", 0);
    } else {
        const esp_app_desc_t *app = esp_app_get_description();
        const ota_release_t *newest = releases != NULL ? &releases[0] : &fallback_release;
        const bool available = compare_versions(app->version, newest->version) < 0;
        portENTER_CRITICAL(&status_lock);
        discovered_releases = releases;
        pending_release = *newest;
        status.release_count = release_count;
        for (size_t i = 0; i < release_count; ++i) {
            const ota_release_t *release = releases != NULL ? &releases[i] : &fallback_release;
            strlcpy(status.releases[i].version, release->version, sizeof(status.releases[i].version));
        }
        strlcpy(status.available_version, newest->version, sizeof(status.available_version));
        status.update_available = available;
        portEXIT_CRITICAL(&status_lock);
        free(manifest);
        set_status(available ? OTA_UPDATE_AVAILABLE : OTA_UPDATE_UP_TO_DATE,
                   available ? "A new release is ready" : "You have the latest release", 0);
    }
    vTaskDelete(NULL);
}

static void install_task(void *argument)
{
    (void)argument;
    ota_release_t release;
    portENTER_CRITICAL(&status_lock);
    release = pending_release;
    portEXIT_CRITICAL(&status_lock);

    esp_http_client_config_t config = {
        .url = release.url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
    };
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        set_status(OTA_UPDATE_ERROR, "Firmware image is too large", 0);
        vTaskDelete(NULL);
        return;
    }
    download_response_t response = {
        .partition = partition,
        .result = ESP_OK,
    };
    config.event_handler = download_http_event;
    config.user_data = &response;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        set_status(OTA_UPDATE_ERROR, "Firmware download failed", 0);
        vTaskDelete(NULL);
        return;
    }
    uint8_t digest[32];
    esp_err_t result = esp_http_client_perform(client);
    if (result == ESP_OK && esp_http_client_get_status_code(client) != 200) result = ESP_FAIL;
    if (result == ESP_OK) result = response.result;
    if (result == ESP_OK && (!response.started || response.downloaded == 0)) result = ESP_FAIL;
    if (response.started && result == ESP_OK && mbedtls_sha256_finish(&response.sha, digest) != 0) result = ESP_FAIL;
    if (response.started) mbedtls_sha256_free(&response.sha);
    esp_http_client_cleanup(client);
    set_status(OTA_UPDATE_VERIFYING, "Verifying firmware", 100);
    if (result == ESP_OK && memcmp(digest, release.sha256, sizeof(digest)) != 0) result = ESP_ERR_INVALID_CRC;
    if (result == ESP_OK) result = esp_ota_end(response.handle);
    else if (response.handle != 0) esp_ota_abort(response.handle);
    if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(result));
        set_status(OTA_UPDATE_ERROR, "Firmware verification failed", 0);
        vTaskDelete(NULL);
        return;
    }
    set_status(OTA_UPDATE_RESTARTING, "Update installed; restarting", 100);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

esp_err_t ota_update_check(void)
{
    ota_update_status_t snapshot;
    ota_update_get_status(&snapshot);
    if (snapshot.state == OTA_UPDATE_CHECKING || snapshot.state == OTA_UPDATE_DOWNLOADING ||
        snapshot.state == OTA_UPDATE_VERIFYING || snapshot.state == OTA_UPDATE_RESTARTING) return ESP_ERR_INVALID_STATE;
    /* A new check cannot overlap installation, so release the previous
     * catalogue before starting another one. */
    free(discovered_releases);
    discovered_releases = NULL;
    portENTER_CRITICAL(&status_lock);
    status.release_count = 0;
    status.update_available = false;
    status.available_version[0] = '\0';
    memset(status.releases, 0, sizeof(status.releases));
    portEXIT_CRITICAL(&status_lock);
    set_status(OTA_UPDATE_CHECKING, "Contacting release server", 0);
    if (create_ota_task(check_task, "ota_check", OTA_CHECK_TASK_STACK_SIZE) == pdPASS) return ESP_OK;
    set_status(OTA_UPDATE_ERROR, "Not enough memory to check", 0);
    return ESP_ERR_NO_MEM;
}

esp_err_t ota_update_install(void)
{
    return ota_update_install_release(0);
}

esp_err_t ota_update_install_release(uint8_t index)
{
    ota_update_status_t snapshot;
    ota_update_get_status(&snapshot);
    if ((snapshot.state != OTA_UPDATE_AVAILABLE && snapshot.state != OTA_UPDATE_UP_TO_DATE) ||
        index >= snapshot.release_count) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&status_lock);
    if (index > 0 && discovered_releases == NULL) {
        portEXIT_CRITICAL(&status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    pending_release = discovered_releases == NULL ? pending_release : discovered_releases[index];
    portEXIT_CRITICAL(&status_lock);
    set_status(OTA_UPDATE_DOWNLOADING, "Downloading firmware", 0);
    if (create_ota_task(install_task, "ota_install", OTA_INSTALL_TASK_STACK_SIZE) == pdPASS) return ESP_OK;
    set_status(OTA_UPDATE_ERROR, "Not enough memory to update", 0);
    return ESP_ERR_NO_MEM;
}

bool ota_update_is_installing(void)
{
    ota_update_status_t snapshot;
    ota_update_get_status(&snapshot);
    return snapshot.state == OTA_UPDATE_DOWNLOADING || snapshot.state == OTA_UPDATE_VERIFYING ||
           snapshot.state == OTA_UPDATE_RESTARTING;
}

void ota_update_release_catalog(void)
{
    ota_update_status_t snapshot;
    ota_update_get_status(&snapshot);
    if (snapshot.state == OTA_UPDATE_CHECKING || ota_update_is_installing()) return;
    free(discovered_releases);
    discovered_releases = NULL;
    portENTER_CRITICAL(&status_lock);
    status.release_count = 0;
    memset(status.releases, 0, sizeof(status.releases));
    portEXIT_CRITICAL(&status_lock);
}
