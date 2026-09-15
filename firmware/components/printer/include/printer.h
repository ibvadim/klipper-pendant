#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PRINTER_CONNECTION_UNCONFIGURED,
    PRINTER_CONNECTION_OFFLINE,
    PRINTER_CONNECTION_ONLINE,
} printer_connection_t;

typedef enum {
    PRINTER_STATE_UNKNOWN,
    PRINTER_STATE_STARTUP,
    PRINTER_STATE_READY,
    PRINTER_STATE_ERROR,
    PRINTER_STATE_SHUTDOWN,
} printer_operational_state_t;

typedef enum {
    PRINTER_JOB_UNKNOWN,
    PRINTER_JOB_STANDBY,
    PRINTER_JOB_PRINTING,
    PRINTER_JOB_PAUSED,
    PRINTER_JOB_COMPLETE,
    PRINTER_JOB_CANCELLED,
    PRINTER_JOB_ERROR,
} printer_job_state_t;

typedef struct {
    char name[33];
    /* Connection to the state source (Moonraker), not Klippy's state. */
    printer_connection_t connection;
    /* Klippy/printer state, valid independently of the Moonraker link. */
    printer_operational_state_t operational_state;
    char state_message[96];
    printer_job_state_t job_state;
    char job_message[96];
    char filename[96];
    /** Print completion in tenths of a percent (0..1000). */
    uint16_t progress_tenths_percent;
    /** Time spent printing the current job, excluding pauses. */
    uint32_t print_duration_seconds;
    int16_t hotend_current_deci_c;
    int16_t hotend_target_deci_c;
    int16_t bed_current_deci_c;
    int16_t bed_target_deci_c;
    /** Toolhead position in hundredths of a millimetre. */
    int32_t toolhead_x_centi_mm;
    int32_t toolhead_y_centi_mm;
    int32_t toolhead_z_centi_mm;
    bool toolhead_position_valid;
    /** Homing state reported by Klipper's toolhead.homed_axes (X, Y, Z). */
    bool toolhead_axes_homed[3];
    /** Live tuning values reported by Klipper's gcode_move/fan objects. */
    uint16_t speed_percent;
    uint16_t flow_percent;
    uint8_t fan_speed_percent;
    int32_t z_offset_centi_mm;
    bool tune_values_valid;
} printer_state_t;

#define PRINTER_MAX_HEATERS 8
#define PRINTER_MAX_FANS 8
#define PRINTER_MAX_ACTIONS 24
#define PRINTER_CAPABILITY_ID_MAX_LEN 32
#define PRINTER_CAPABILITY_LABEL_MAX_LEN 24

typedef struct {
    char id[PRINTER_CAPABILITY_ID_MAX_LEN + 1];
    char label[PRINTER_CAPABILITY_LABEL_MAX_LEN + 1];
    int16_t current_deci_c;
    int16_t target_deci_c;
    int16_t min_deci_c;
    int16_t max_deci_c;
    bool available;
    bool controllable;
} printer_heater_capability_t;

typedef struct {
    char id[PRINTER_CAPABILITY_ID_MAX_LEN + 1];
    char label[PRINTER_CAPABILITY_LABEL_MAX_LEN + 1];
    uint8_t speed_percent;
    bool available;
    bool controllable;
} printer_fan_capability_t;

typedef enum {
    PRINTER_ACTION_FILAMENT,
    PRINTER_ACTION_MOTION,
    PRINTER_ACTION_SYSTEM,
    PRINTER_ACTION_CUSTOM,
} printer_action_group_t;

typedef struct {
    char id[PRINTER_CAPABILITY_ID_MAX_LEN + 1];
    char label[PRINTER_CAPABILITY_LABEL_MAX_LEN + 1];
    printer_action_group_t group;
    bool available;
    bool dangerous;
} printer_action_capability_t;

/** Normalized printer features consumed by UI screens, never raw Klipper JSON. */
typedef struct {
    printer_heater_capability_t heaters[PRINTER_MAX_HEATERS];
    size_t heater_count;
    printer_fan_capability_t fans[PRINTER_MAX_FANS];
    size_t fan_count;
    printer_action_capability_t actions[PRINTER_MAX_ACTIONS];
    size_t action_count;
    bool speed_factor;
    bool flow_factor;
    bool files;
    bool pause;
    bool axes[3];
} printer_capabilities_t;

typedef struct {
    esp_err_t (*get_state)(printer_state_t *state, void *context);
    esp_err_t (*get_capabilities)(printer_capabilities_t *capabilities, void *context);
    void *context;
} printer_source_t;

esp_err_t printer_init(void);
esp_err_t printer_get_state(printer_state_t *state);
esp_err_t printer_get_capabilities(printer_capabilities_t *capabilities);
esp_err_t printer_set_source(const printer_source_t *source);
void printer_use_mock_source(void);
