#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_UPDATE_VERSION_MAX_LEN 32
#define OTA_UPDATE_MESSAGE_MAX_LEN 96
#define OTA_UPDATE_MAX_RELEASES 8

typedef enum {
    OTA_UPDATE_IDLE,
    OTA_UPDATE_CHECKING,
    OTA_UPDATE_AVAILABLE,
    OTA_UPDATE_UP_TO_DATE,
    OTA_UPDATE_DOWNLOADING,
    OTA_UPDATE_VERIFYING,
    OTA_UPDATE_RESTARTING,
    OTA_UPDATE_ERROR,
} ota_update_state_t;

typedef struct {
    char version[OTA_UPDATE_VERSION_MAX_LEN];
} ota_update_release_info_t;

typedef struct {
    ota_update_state_t state;
    char current_version[OTA_UPDATE_VERSION_MAX_LEN];
    char available_version[OTA_UPDATE_VERSION_MAX_LEN];
    char message[OTA_UPDATE_MESSAGE_MAX_LEN];
    uint8_t progress;
    bool update_available;
    uint8_t release_count;
    ota_update_release_info_t releases[OTA_UPDATE_MAX_RELEASES];
} ota_update_status_t;

/** Starts a background request for the published release list and manifests. */
esp_err_t ota_update_check(void);

/** Downloads the newest release selected by ota_update_check and reboots on success. */
esp_err_t ota_update_install(void);

/** Downloads a checked release by its index in ota_update_status_t.releases. */
esp_err_t ota_update_install_release(uint8_t index);

/** True from the start of download until the device restarts or reports an error. */
bool ota_update_is_installing(void);

/** Releases the cached checked-release metadata after leaving Firmware. */
void ota_update_release_catalog(void);

void ota_update_get_status(ota_update_status_t *status);
const char *ota_update_state_name(ota_update_state_t state);
