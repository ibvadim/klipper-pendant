#pragma once

#include "esp_err.h"
#include "printer.h"

typedef enum {
    MOONRAKER_DISABLED,
    MOONRAKER_WIFI_WAIT,
    MOONRAKER_CONNECTING,
    MOONRAKER_IDENTIFYING,
    MOONRAKER_KLIPPY_WAIT,
    MOONRAKER_SUBSCRIBING,
    MOONRAKER_ONLINE,
    MOONRAKER_KLIPPY_ERROR,
    MOONRAKER_BACKOFF,
    MOONRAKER_AUTH_ERROR,
    MOONRAKER_PROTOCOL_ERROR,
} moonraker_connection_state_t;

typedef struct {
    moonraker_connection_state_t state;
    char detail[96];
    uint32_t reconnect_delay_ms;
    uint32_t last_update_ms;
} moonraker_status_t;

#define MOONRAKER_FILE_NAME_MAX_LEN 80
#define MOONRAKER_FILE_BROWSER_PAGE_SIZE 20
#define MOONRAKER_MAX_MACROS 32
#define MOONRAKER_MACRO_NAME_MAX_LEN 48
/* Keep this bounded cache small: WebSocket and TLS transport buffers also
 * require internal ESP32 RAM while the printer is connected. */
#define MOONRAKER_HISTORY_PAGE_SIZE 8
#define MOONRAKER_HISTORY_FILENAME_MAX_LEN 96
#define MOONRAKER_HISTORY_MESSAGE_MAX_LEN 96
#define MOONRAKER_CONSOLE_LINES 32
#define MOONRAKER_CONSOLE_LINE_MAX_LEN 144
#define MOONRAKER_PROMPT_TEXT_MAX_LEN 384
#define MOONRAKER_PROMPT_MAX_BUTTONS 8
#define MOONRAKER_PROMPT_LABEL_MAX_LEN 32
#define MOONRAKER_PROMPT_COMMAND_MAX_LEN 160
#define MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX 32
#define MOONRAKER_EXCLUDE_OBJECT_NAME_MAX_LEN 48

typedef struct {
    char name[MOONRAKER_EXCLUDE_OBJECT_NAME_MAX_LEN + 1];
    float center_x, center_y;
    float polygon[MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX][2];
    uint8_t polygon_count;
    bool excluded, current;
} moonraker_exclude_object_t;

typedef struct {
    moonraker_exclude_object_t *objects;
    size_t count;
    uint32_t generation;
    bool loading, valid, unavailable, truncated;
    char error[64];
} moonraker_exclude_objects_t;

/* The console is deliberately a short, bounded G-code journal.  It is not a
 * replacement for klippy.log, which would be unsafe to retain in ESP32 RAM. */
typedef struct {
    char lines[MOONRAKER_CONSOLE_LINES][MOONRAKER_CONSOLE_LINE_MAX_LEN];
    size_t count;
    uint32_t generation;
    bool loading;
    bool trimmed;
    char error[64];
} moonraker_console_t;

typedef struct {
    char label[MOONRAKER_PROMPT_LABEL_MAX_LEN];
    char command[MOONRAKER_PROMPT_COMMAND_MAX_LEN];
    char style[12];
    bool footer;
} moonraker_prompt_button_t;

/* Parsed action:prompt_* state.  It is deliberately bounded because macro
 * authors control every string and the pendant must not grow RAM usage with
 * an unbounded sequence of RESPOND commands. */
typedef struct {
    char title[64];
    char text[MOONRAKER_PROMPT_TEXT_MAX_LEN];
    moonraker_prompt_button_t buttons[MOONRAKER_PROMPT_MAX_BUTTONS];
    size_t button_count;
    uint32_t generation;
    bool visible;
    bool truncated;
} moonraker_prompt_t;

typedef struct {
    bool is_active;
    bool position_valid;
    float z_position;
    bool lower_valid;
    float z_position_lower;
    bool upper_valid;
    float z_position_upper;
    uint32_t generation;
} moonraker_manual_probe_t;

typedef struct {
    char id[48];
    char filename[MOONRAKER_HISTORY_FILENAME_MAX_LEN];
    char status[16];
    char message[MOONRAKER_HISTORY_MESSAGE_MAX_LEN];
    uint64_t start_time_ms;
    uint64_t end_time_ms;
    uint32_t duration_seconds;
    uint32_t filament_used_mm;
    bool file_exists;
} moonraker_history_job_t;

typedef struct {
    size_t offset;
    size_t count;
    size_t total;
    uint32_t generation;
    bool loading;
    bool valid;
    bool unavailable;
    char error[64];
} moonraker_history_status_t;

/* Aggregate values from Moonraker's server.history.totals endpoint.
 * Durations are seconds; filament length is millimetres. */
typedef struct {
    uint64_t total_jobs;
    uint64_t total_time_seconds;
    uint64_t total_print_time_seconds;
    uint64_t total_filament_used_mm;
    uint64_t longest_print_seconds;
    uint32_t generation;
    bool loading;
    bool valid;
    char error[64];
} moonraker_history_totals_t;

typedef struct {
    char name[MOONRAKER_FILE_NAME_MAX_LEN + 1];
    char filament_type[24];
    char filament_name[40];
    uint32_t size_bytes;
    uint32_t estimated_time_seconds;
    uint64_t modified_ms;
    uint16_t filament_weight_deci_g;
    bool is_directory;
    bool has_thumbnail;
} moonraker_file_entry_t;

typedef struct {
    char path[96];
    size_t entry_count;
    uint32_t generation;
    bool loading;
    bool valid;
    char error[64];
} moonraker_file_browser_t;

/* A bounded view into the dynamically held directory listing. */
typedef struct {
    moonraker_file_entry_t entries[MOONRAKER_FILE_BROWSER_PAGE_SIZE];
    size_t offset;
    size_t entry_count;
} moonraker_file_browser_page_t;

typedef struct {
    char path[96];
    moonraker_file_entry_t file;
    uint32_t generation;
    bool loading;
    bool valid;
    char error[64];
} moonraker_file_metadata_t;

typedef struct {
    char names[MOONRAKER_MAX_MACROS][MOONRAKER_MACRO_NAME_MAX_LEN + 1];
    size_t count;
    uint32_t generation;
    bool loading;
    bool valid;
    char error[64];
} moonraker_macro_list_t;

/** Capacity of the filesystem containing Moonraker's G-code root. */
typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t generation;
    bool loading;
    bool valid;
    char error[64];
} moonraker_disk_info_t;

esp_err_t moonraker_init(void);
/** Temporarily close the WebSocket and stop polling while OTA owns Wi-Fi/RAM. */
void moonraker_set_suspended(bool suspended);
/** True once the Moonraker task has closed its socket and released buffers. */
bool moonraker_is_suspended(void);
void moonraker_get_status(moonraker_status_t *status);
esp_err_t moonraker_get_printer_state(printer_state_t *state, void *context);
esp_err_t moonraker_request_directory(const char *path);
void moonraker_get_file_browser(moonraker_file_browser_t *browser);
void moonraker_get_file_browser_page(size_t offset, moonraker_file_browser_page_t *page);
bool moonraker_get_file_browser_entry(size_t index, moonraker_file_entry_t *entry);
esp_err_t moonraker_request_file_metadata(const char *path);
void moonraker_get_file_metadata(moonraker_file_metadata_t *metadata);
esp_err_t moonraker_request_macros(void);
void moonraker_get_macros(moonraker_macro_list_t *macros);
esp_err_t moonraker_request_disk_info(void);
void moonraker_get_disk_info(moonraker_disk_info_t *info);
esp_err_t moonraker_request_history(size_t offset);
void moonraker_get_history_status(moonraker_history_status_t *status);
bool moonraker_get_history_job(size_t index, moonraker_history_job_t *job);
esp_err_t moonraker_request_history_totals(void);
void moonraker_get_history_totals(moonraker_history_totals_t *totals);
/** Releases the on-demand history page cache when the user leaves History. */
void moonraker_release_history(void);
esp_err_t moonraker_request_gcode_store(void);
void moonraker_get_console(moonraker_console_t *console);
void moonraker_get_prompt(moonraker_prompt_t *prompt);
void moonraker_dismiss_prompt(void);
void moonraker_get_manual_probe(moonraker_manual_probe_t *probe);
esp_err_t moonraker_request_exclude_objects(void);
void moonraker_get_exclude_objects(moonraker_exclude_objects_t *objects);
/** Optimistically reflect a successfully queued exclusion until Klipper's
 * subscribed status update confirms it. */
void moonraker_mark_object_excluded(const char *name);
/** Releases the transient object-map cache when Exclude Objects is closed. */
void moonraker_release_exclude_objects(void);
/** Add a pendant-originated command to the bounded console journal. */
void moonraker_console_record_local(const char *text);
esp_err_t moonraker_print_start(const char *filename);
esp_err_t moonraker_print_pause(void);
esp_err_t moonraker_print_resume(void);
esp_err_t moonraker_print_cancel(void);
/**
 * Send Moonraker's dedicated HTTP emergency-stop request.  This deliberately
 * bypasses the G-code/WebSocket queue, so it must never be replaced with M112.
 */
esp_err_t moonraker_emergency_stop(void);
/** Execute a bounded G-code script through Moonraker's printer.gcode.script API. */
esp_err_t moonraker_send_gcode(const char *script);
/** Restart the Moonraker server process. */
esp_err_t moonraker_restart_server(void);
/** Reboot or power off the host running Moonraker. */
esp_err_t moonraker_reboot_system(void);
esp_err_t moonraker_shutdown_system(void);
