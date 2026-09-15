#include "moonraker.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi_manager.h"

#define TAG "moonraker"
#define RECONCILE_MS 60000U
#define STALE_UPDATE_MS 15000U
#define RPC_RESPONSE_TIMEOUT_MS 10000U
#define MAX_MESSAGE_SIZE (1024U * 1024U)

static SemaphoreHandle_t state_lock;
static esp_websocket_client_handle_t websocket;
static printer_state_t cached_printer;
static moonraker_status_t client_status = { .state = MOONRAKER_DISABLED };
static volatile bool socket_closed;
static volatile bool socket_connected;
static uint32_t request_id;
static int identify_request_id = -1;
static int server_info_request_id = -1;
static int subscription_request_id = -1;
static uint32_t connect_started_ms;
static uint32_t request_started_ms;
static volatile bool identify_pending;
static volatile bool server_info_pending;
static volatile bool subscription_pending;
static char *message_buffer;
static size_t message_size;
static char active_uri[128];
static moonraker_file_browser_t file_browser = { .path = "gcodes" };
/* The browser only needs these fields until the user selects a file.  The
 * richer metadata is loaded lazily for that one file. */
typedef struct {
    char name[MOONRAKER_FILE_NAME_MAX_LEN + 1];
    uint32_t size_bytes;
    uint64_t modified_ms;
    bool is_directory;
} file_browser_list_entry_t;
static file_browser_list_entry_t *file_browser_entries;
static int directory_request_id = -1;
static moonraker_file_metadata_t file_metadata;
static int metadata_request_id = -1;
static moonraker_macro_list_t macros;
static int macros_request_id = -1;
static moonraker_disk_info_t disk_info;
static int disk_info_request_id = -1;
static moonraker_history_status_t history;
static moonraker_history_totals_t history_totals;
static moonraker_history_job_t *history_jobs;
static int history_request_id = -1;
static int history_totals_request_id = -1;
static int gcode_request_id = -1;
static int gcode_store_request_id = -1;
static moonraker_exclude_objects_t *exclude_objects;
static int exclude_objects_request_id = -1;
static int ignored_exclude_objects_request_id = -1;
static moonraker_console_t console;
static moonraker_prompt_t prompt;
static moonraker_manual_probe_t manual_probe;
static volatile bool file_list_changed;
static volatile bool moonraker_suspended;
static volatile bool moonraker_suspend_complete;

/* Object markers can contain hundreds of polygon points.  cJSON represents
 * every number and array item as a separate allocation; keeping those small
 * allocations in internal RAM makes an otherwise modest Moonraker response
 * fail while LVGL is active.  This component is the sole cJSON user, so route
 * its transient parse tree to the board's PSRAM. */
static void *moonraker_json_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void moonraker_json_free(void *pointer)
{
    heap_caps_free(pointer);
}

static int compare_file_entries(const void *left_ptr, const void *right_ptr)
{
    const file_browser_list_entry_t *left = left_ptr;
    const file_browser_list_entry_t *right = right_ptr;
    /* Directories are navigation targets, so keep the whole directory block
     * ahead of printable files regardless of their modification time. */
    if (left->is_directory != right->is_directory) return left->is_directory ? -1 : 1;
    if (left->modified_ms < right->modified_ms) return 1;
    if (left->modified_ms > right->modified_ms) return -1;
    return strcasecmp(left->name, right->name);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void console_append_locked(const char *prefix, const char *text)
{
    if (text == NULL || text[0] == '\0') return;
    char rendered[MOONRAKER_CONSOLE_LINE_MAX_LEN];
    snprintf(rendered, sizeof(rendered), "%s%s", prefix == NULL ? "" : prefix, text);
    if (console.count == MOONRAKER_CONSOLE_LINES) {
        memmove(console.lines, console.lines + 1, sizeof(console.lines[0]) *
                (MOONRAKER_CONSOLE_LINES - 1));
        console.count--;
        console.trimmed = true;
    }
    strlcpy(console.lines[console.count++], rendered, sizeof(console.lines[0]));
    ++console.generation;
}

static void console_append(const char *prefix, const char *text)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    console_append_locked(prefix, text);
    xSemaphoreGive(state_lock);
}

static const char *prompt_action(const char *response)
{
    if (response == NULL) return NULL;
    while (*response == ' ' || *response == '\t') ++response;
    if (response[0] == '/' && response[1] == '/') {
        response += 2;
        while (*response == ' ' || *response == '\t') ++response;
    }
    return strncmp(response, "action:prompt_", 14) == 0 ? response + 14 : NULL;
}

static void prompt_append_text_locked(const char *text)
{
    const size_t used = strlen(prompt.text);
    const size_t separator = used > 0 ? 1 : 0;
    if (used + separator + strlen(text) >= sizeof(prompt.text)) prompt.truncated = true;
    if (separator && used + 1 < sizeof(prompt.text)) strlcat(prompt.text, "\n", sizeof(prompt.text));
    strlcat(prompt.text, text, sizeof(prompt.text));
}

static void apply_prompt_response(const char *response)
{
    const char *action = prompt_action(response);
    if (action == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (strncmp(action, "begin", 5) == 0 && (action[5] == '\0' || action[5] == ' ')) {
        const uint32_t generation = prompt.generation + 1;
        memset(&prompt, 0, sizeof(prompt));
        prompt.generation = generation;
        const char *title = action + 5;
        while (*title == ' ') ++title;
        strlcpy(prompt.title, *title ? title : "Prompt", sizeof(prompt.title));
    } else if (strncmp(action, "text", 4) == 0 && (action[4] == '\0' || action[4] == ' ')) {
        const char *text = action + 4;
        while (*text == ' ') ++text;
        prompt_append_text_locked(text);
        ++prompt.generation;
    } else if ((strncmp(action, "button", 6) == 0 && (action[6] == '\0' || action[6] == ' ')) ||
               (strncmp(action, "footer_button", 13) == 0 &&
                (action[13] == '\0' || action[13] == ' '))) {
        const bool footer = action[0] == 'f';
        const char *definition = action + (footer ? 13 : 6);
        while (*definition == ' ') ++definition;
        if (prompt.button_count < MOONRAKER_PROMPT_MAX_BUTTONS) {
            char fields[MOONRAKER_PROMPT_LABEL_MAX_LEN + MOONRAKER_PROMPT_COMMAND_MAX_LEN + 16];
            strlcpy(fields, definition, sizeof(fields));
            char *command = strchr(fields, '|');
            char *style = NULL;
            if (command != NULL) {
                *command++ = '\0';
                style = strchr(command, '|');
                if (style != NULL) *style++ = '\0';
            }
            moonraker_prompt_button_t *button = &prompt.buttons[prompt.button_count++];
            strlcpy(button->label, fields[0] ? fields : "Run", sizeof(button->label));
            strlcpy(button->command, command == NULL ? "" : command, sizeof(button->command));
            strlcpy(button->style, style == NULL ? "primary" : style, sizeof(button->style));
            button->footer = footer;
        } else {
            prompt.truncated = true;
        }
        ++prompt.generation;
    } else if (strcmp(action, "show") == 0) {
        prompt.visible = true;
        ++prompt.generation;
    } else if (strcmp(action, "end") == 0) {
        prompt.visible = false;
        ++prompt.generation;
    }
    /* button_group_start/end are accepted but need no state on this compact,
     * single-column display. */
    xSemaphoreGive(state_lock);
}

static void apply_gcode_store_result(cJSON *result)
{
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(result, "gcode_store");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    console.count = 0;
    console.trimmed = false;
    console.loading = false;
    console.error[0] = '\0';
    if (cJSON_IsArray(entries)) {
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, entries) {
            cJSON *message = cJSON_GetObjectItemCaseSensitive(entry, "message");
            cJSON *type = cJSON_GetObjectItemCaseSensitive(entry, "type");
            if (cJSON_IsString(message)) {
                console_append_locked(cJSON_IsString(type) && strcmp(type->valuestring, "command") == 0 ? "> " : "< ",
                                      message->valuestring);
            }
        }
    }
    ++console.generation;
    xSemaphoreGive(state_lock);
}

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t increase_backoff(uint32_t backoff_ms)
{
    return backoff_ms >= 15000U ? 30000U : backoff_ms * 2U;
}

static void set_reconnect_delay(uint32_t delay_ms)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    client_status.reconnect_delay_ms = delay_ms;
    xSemaphoreGive(state_lock);
}

static void set_state(moonraker_connection_state_t state, const char *detail)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    client_status.state = state;
    strlcpy(client_status.detail, detail == NULL ? "" : detail, sizeof(client_status.detail));
    xSemaphoreGive(state_lock);
}

static void set_connection(printer_connection_t connection)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cached_printer.connection = connection;
    if (connection != PRINTER_CONNECTION_ONLINE) {
        cached_printer.operational_state = PRINTER_STATE_UNKNOWN;
        cached_printer.job_state = PRINTER_JOB_UNKNOWN;
        cached_printer.state_message[0] = '\0';
        cached_printer.job_message[0] = '\0';
        cached_printer.filename[0] = '\0';
        cached_printer.progress_tenths_percent = 0;
        cached_printer.print_duration_seconds = 0;
        cached_printer.hotend_current_deci_c = 0;
        cached_printer.hotend_target_deci_c = 0;
        cached_printer.bed_current_deci_c = 0;
        cached_printer.bed_target_deci_c = 0;
        cached_printer.toolhead_x_centi_mm = 0;
        cached_printer.toolhead_y_centi_mm = 0;
        cached_printer.toolhead_z_centi_mm = 0;
        cached_printer.toolhead_position_valid = false;
        memset(cached_printer.toolhead_axes_homed, 0, sizeof(cached_printer.toolhead_axes_homed));
        cached_printer.speed_percent = 0;
        cached_printer.flow_percent = 0;
        cached_printer.fan_speed_percent = 0;
        cached_printer.z_offset_centi_mm = 0;
        cached_printer.tune_values_valid = false;
        exclude_objects_request_id = -1;
        if (exclude_objects != NULL) {
            exclude_objects->loading = false;
            exclude_objects->valid = false;
            exclude_objects->unavailable = false;
            exclude_objects->count = 0;
            ++exclude_objects->generation;
        }
        client_status.last_update_ms = 0;
    }
    xSemaphoreGive(state_lock);
}

static void apply_klippy_state_locked(const char *state, const char *message)
{
    if (state == NULL) {
        cached_printer.operational_state = PRINTER_STATE_UNKNOWN;
    } else if (strcmp(state, "ready") == 0) {
        cached_printer.operational_state = PRINTER_STATE_READY;
    } else if (strcmp(state, "startup") == 0) {
        cached_printer.operational_state = PRINTER_STATE_STARTUP;
    } else if (strcmp(state, "error") == 0) {
        cached_printer.operational_state = PRINTER_STATE_ERROR;
    } else if (strcmp(state, "shutdown") == 0) {
        cached_printer.operational_state = PRINTER_STATE_SHUTDOWN;
    } else {
        cached_printer.operational_state = PRINTER_STATE_UNKNOWN;
    }
    if (cached_printer.operational_state != PRINTER_STATE_READY) {
        cached_printer.job_state = PRINTER_JOB_UNKNOWN;
        cached_printer.job_message[0] = '\0';
    }
    if (message != NULL) {
        strlcpy(cached_printer.state_message, message, sizeof(cached_printer.state_message));
    } else if (cached_printer.operational_state == PRINTER_STATE_ERROR &&
               cached_printer.state_message[0] == '\0') {
        strlcpy(cached_printer.state_message, "Klipper error", sizeof(cached_printer.state_message));
    } else if (cached_printer.operational_state == PRINTER_STATE_SHUTDOWN &&
               cached_printer.state_message[0] == '\0') {
        strlcpy(cached_printer.state_message, "Klipper shutdown", sizeof(cached_printer.state_message));
    } else if (cached_printer.operational_state != PRINTER_STATE_ERROR &&
               cached_printer.operational_state != PRINTER_STATE_SHUTDOWN) {
        cached_printer.state_message[0] = '\0';
    }
}

static void set_klippy_state(const char *state, const char *message)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    apply_klippy_state_locked(state, message);
    xSemaphoreGive(state_lock);
}

static printer_job_state_t parse_job_state(const char *state)
{
    if (state == NULL) return PRINTER_JOB_UNKNOWN;
    if (strcmp(state, "standby") == 0) return PRINTER_JOB_STANDBY;
    if (strcmp(state, "printing") == 0) return PRINTER_JOB_PRINTING;
    if (strcmp(state, "paused") == 0) return PRINTER_JOB_PAUSED;
    if (strcmp(state, "complete") == 0) return PRINTER_JOB_COMPLETE;
    if (strcmp(state, "cancelled") == 0) return PRINTER_JOB_CANCELLED;
    if (strcmp(state, "error") == 0) return PRINTER_JOB_ERROR;
    return PRINTER_JOB_UNKNOWN;
}

static cJSON *object_item(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsObject(item) ? item : NULL;
}

static void update_temperature(cJSON *object, int16_t *current, int16_t *target)
{
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(object, "temperature");
    cJSON *target_value = cJSON_GetObjectItemCaseSensitive(object, "target");
    if (cJSON_IsNumber(temperature)) *current = (int16_t)(temperature->valuedouble * 10.0f);
    if (cJSON_IsNumber(target_value)) *target = (int16_t)(target_value->valuedouble * 10.0f);
}

static void update_progress(cJSON *object)
{
    cJSON *progress = cJSON_GetObjectItemCaseSensitive(object, "progress");
    if (cJSON_IsNumber(progress)) {
        float value = progress->valuedouble * 1000.0f;
        cached_printer.progress_tenths_percent = (uint16_t)(value < 0.0f ? 0.0f : value > 1000.0f ? 1000.0f : value);
    }
}

static void update_print_duration(cJSON *object)
{
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(object, "print_duration");
    if (!cJSON_IsNumber(duration)) return;
    double seconds = duration->valuedouble;
    cached_printer.print_duration_seconds = (uint32_t)(seconds < 0.0 ? 0.0 :
        seconds > (double)UINT32_MAX ? (double)UINT32_MAX : seconds);
}

static void update_toolhead_position(cJSON *object)
{
    cJSON *position = cJSON_GetObjectItemCaseSensitive(object, "position");
    if (!cJSON_IsArray(position) || cJSON_GetArraySize(position) < 3) return;
    cJSON *x = cJSON_GetArrayItem(position, 0);
    cJSON *y = cJSON_GetArrayItem(position, 1);
    cJSON *z = cJSON_GetArrayItem(position, 2);
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(z)) return;
    cached_printer.toolhead_x_centi_mm = (int32_t)(x->valuedouble * 100.0f);
    cached_printer.toolhead_y_centi_mm = (int32_t)(y->valuedouble * 100.0f);
    cached_printer.toolhead_z_centi_mm = (int32_t)(z->valuedouble * 100.0f);
    cached_printer.toolhead_position_valid = true;
    cJSON *homed_axes = cJSON_GetObjectItemCaseSensitive(object, "homed_axes");
    if (cJSON_IsString(homed_axes)) {
        cached_printer.toolhead_axes_homed[0] = strchr(homed_axes->valuestring, 'x') != NULL;
        cached_printer.toolhead_axes_homed[1] = strchr(homed_axes->valuestring, 'y') != NULL;
        cached_printer.toolhead_axes_homed[2] = strchr(homed_axes->valuestring, 'z') != NULL;
    }
}

static void update_tune_values(cJSON *gcode_move, cJSON *fan)
{
    if (gcode_move != NULL) {
        cJSON *speed_factor = cJSON_GetObjectItemCaseSensitive(gcode_move, "speed_factor");
        cJSON *extrude_factor = cJSON_GetObjectItemCaseSensitive(gcode_move, "extrude_factor");
        cJSON *homing_origin = cJSON_GetObjectItemCaseSensitive(gcode_move, "homing_origin");
        if (cJSON_IsNumber(speed_factor)) {
            cached_printer.speed_percent = (uint16_t)(speed_factor->valuedouble * 100.0f);
            cached_printer.tune_values_valid = true;
        }
        if (cJSON_IsNumber(extrude_factor)) {
            cached_printer.flow_percent = (uint16_t)(extrude_factor->valuedouble * 100.0f);
            cached_printer.tune_values_valid = true;
        }
        if (cJSON_IsArray(homing_origin) && cJSON_GetArraySize(homing_origin) >= 3) {
            cJSON *z = cJSON_GetArrayItem(homing_origin, 2);
            if (cJSON_IsNumber(z)) cached_printer.z_offset_centi_mm =
                (int32_t)(z->valuedouble * 100.0f);
        }
    }
    if (fan != NULL) {
        cJSON *speed = cJSON_GetObjectItemCaseSensitive(fan, "speed");
        if (cJSON_IsNumber(speed)) {
            float percent = speed->valuedouble * 100.0f;
            cached_printer.fan_speed_percent = (uint8_t)(percent < 0.0f ? 0.0f :
                percent > 100.0f ? 100.0f : percent);
        }
    }
}

static bool exclude_name_is_listed(cJSON *names, const char *name)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, names) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0) return true;
    }
    return false;
}

/* state_lock must be held by the caller.  Polygons are intentionally clipped
 * here, before they enter the UI model, so a pathological G-code cannot turn
 * into an unbounded RAM allocation on the pendant. */
static void update_exclude_objects_locked(cJSON *object)
{
    if (exclude_objects == NULL || !cJSON_IsObject(object)) return;
    cJSON *definitions = cJSON_GetObjectItemCaseSensitive(object, "objects");
    cJSON *excluded = cJSON_GetObjectItemCaseSensitive(object, "excluded_objects");
    cJSON *current = cJSON_GetObjectItemCaseSensitive(object, "current_object");
    /* Subscription updates intentionally contain only the two changing state
     * fields.  Merge those into the cached geometry instead of replacing the
     * complete object list with an empty one. */
    if (!cJSON_IsArray(definitions)) {
        if (!cJSON_IsArray(excluded) && current == NULL) return;
        for (size_t i = 0; i < exclude_objects->count; ++i) {
            moonraker_exclude_object_t *entry = &exclude_objects->objects[i];
            if (cJSON_IsArray(excluded)) entry->excluded = exclude_name_is_listed(excluded, entry->name);
            if (current != NULL) entry->current = cJSON_IsString(current) &&
                                                  strcmp(current->valuestring, entry->name) == 0;
        }
        ++exclude_objects->generation;
        return;
    }
    cJSON *definition = NULL;
    size_t count = 0;
    cJSON_ArrayForEach(definition, definitions) if (cJSON_IsObject(definition)) ++count;
    moonraker_exclude_object_t *parsed = count == 0 ? NULL :
        heap_caps_calloc(count, sizeof(*parsed), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (count != 0 && parsed == NULL) {
        exclude_objects->loading = false;
        exclude_objects->valid = false;
        strlcpy(exclude_objects->error, "Not enough memory for object map", sizeof(exclude_objects->error));
        ++exclude_objects->generation;
        return;
    }
    free(exclude_objects->objects);
    exclude_objects->objects = parsed;
    exclude_objects->count = 0;
    exclude_objects->truncated = false;
    cJSON_ArrayForEach(definition, definitions) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(definition, "name");
        if (!cJSON_IsString(name)) continue;
        moonraker_exclude_object_t *entry = &exclude_objects->objects[exclude_objects->count++];
        strlcpy(entry->name, name->valuestring, sizeof(entry->name));
        entry->excluded = exclude_name_is_listed(excluded, entry->name);
        entry->current = cJSON_IsString(current) && strcmp(current->valuestring, entry->name) == 0;
        cJSON *center = cJSON_GetObjectItemCaseSensitive(definition, "center");
        cJSON *x = cJSON_IsArray(center) ? cJSON_GetArrayItem(center, 0) : NULL;
        cJSON *y = cJSON_IsArray(center) ? cJSON_GetArrayItem(center, 1) : NULL;
        if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) { entry->center_x = x->valuedouble; entry->center_y = y->valuedouble; }
        cJSON *polygon = cJSON_GetObjectItemCaseSensitive(definition, "polygon");
        cJSON *point = NULL;
        cJSON_ArrayForEach(point, polygon) {
            if (entry->polygon_count == MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX) break;
            x = cJSON_GetArrayItem(point, 0); y = cJSON_GetArrayItem(point, 1);
            if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) continue;
            entry->polygon[entry->polygon_count][0] = x->valuedouble;
            entry->polygon[entry->polygon_count++][1] = y->valuedouble;
        }
    }
    exclude_objects->loading = false; exclude_objects->valid = true;
    exclude_objects->unavailable = false; exclude_objects->error[0] = '\0';
    ++exclude_objects->generation;
}

/* Some slicers emit very dense object polygons.  Handle the matching RPC
 * response directly: retain names, centers, and an evenly sampled outline
 * instead of expanding every coordinate into a cJSON tree.  This keeps the
 * object map while avoiding a partially parsed list when memory is fragmented
 * while LVGL is active.  json must be a NUL-terminated complete message. */
static bool parse_raw_point(const char *cursor, const char *end, float *x, float *y,
                            const char **next)
{
    while (cursor < end && *cursor != '[') ++cursor;
    while (cursor < end) {
        char *number_end = NULL;
        const double parsed_x = strtod(cursor + 1, &number_end);
        if (number_end != cursor + 1 && number_end < end && *number_end == ',') {
            char *point_end = NULL;
            const double parsed_y = strtod(number_end + 1, &point_end);
            if (point_end != number_end + 1 && point_end < end && *point_end == ']') {
                *x = (float)parsed_x;
                *y = (float)parsed_y;
                *next = point_end + 1;
                return true;
            }
        }
        cursor = memchr(cursor + 1, '[', (size_t)(end - cursor - 1));
        if (cursor == NULL) break;
    }
    return false;
}

static const char *raw_array_end(const char *begin, const char *limit)
{
    if (begin == NULL || begin >= limit || *begin != '[') return NULL;
    unsigned depth = 0;
    for (const char *cursor = begin; cursor < limit; ++cursor) {
        if (*cursor == '[') ++depth;
        else if (*cursor == ']' && --depth == 0) return cursor;
    }
    return NULL;
}

static void recover_raw_geometry(moonraker_exclude_object_t *object,
                                 const char *begin, const char *end)
{
    const char *center = strstr(begin, "\"center\":[");
    if (center != NULL && center < end) {
        const char *next = NULL;
        (void)parse_raw_point(center + strlen("\"center\":"), end,
                              &object->center_x, &object->center_y, &next);
    }
    const char *polygon = strstr(begin, "\"polygon\":[");
    if (polygon == NULL || polygon >= end) return;
    const char *outer = polygon + strlen("\"polygon\":");
    const char *polygon_end = raw_array_end(outer, end);
    if (polygon_end == NULL) return;
    const char *points = outer + 1;
    const char *cursor = points;
    const char *next = NULL;
    float x, y;
    size_t point_count = 0;
    while (parse_raw_point(cursor, polygon_end, &x, &y, &next)) {
        ++point_count;
        cursor = next;
    }
    if (point_count < 3) return;

    const size_t kept = point_count < MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX ?
                        point_count : MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX;
    cursor = points;
    size_t source_index = 0;
    size_t kept_index = 0;
    while (kept_index < kept && parse_raw_point(cursor, polygon_end, &x, &y, &next)) {
        const size_t wanted = kept == 1 ? 0 :
                              kept_index * (point_count - 1) / (kept - 1);
        if (source_index == wanted) {
            object->polygon[kept_index][0] = x;
            object->polygon[kept_index][1] = y;
            ++kept_index;
        }
        ++source_index;
        cursor = next;
    }
    object->polygon_count = (uint8_t)kept_index;
}

static bool recover_exclude_objects(const char *json)
{
    if (json == NULL || exclude_objects_request_id < 0) return false;
    const char *id = strstr(json, "\"id\":");
    if (id == NULL || strtol(id + strlen("\"id\":"), NULL, 10) != exclude_objects_request_id) return false;
    const char *objects = strstr(json, "\"objects\":[");
    const char *end = objects == NULL ? NULL : strstr(objects, "],\"excluded_objects\"");
    if (objects == NULL || end == NULL) return false;

    const char *cursor = objects;
    size_t count = 0;
    while ((cursor = strstr(cursor, "\"name\":\"")) != NULL && cursor < end) {
        ++count;
        cursor += strlen("\"name\":\"");
    }
    moonraker_exclude_object_t *parsed = count == 0 ? NULL :
        heap_caps_calloc(count, sizeof(*parsed), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (count != 0 && parsed == NULL) return false;

    const char *excluded = strstr(end, "\"excluded_objects\":[");
    const char *current = strstr(end, "\"current_object\":");
    cursor = objects;
    size_t index = 0;
    while ((cursor = strstr(cursor, "\"name\":\"")) != NULL && cursor < end && index < count) {
        const char *definition = cursor;
        cursor += strlen("\"name\":\"");
        char *destination = parsed[index].name;
        size_t written = 0;
        while (cursor < end && *cursor != '\0' && *cursor != '\"' &&
               written < sizeof(parsed[index].name) - 1) {
            /* Object names are normally identifiers; preserve the common
             * escaped character form as well without an unbounded decoder. */
            if (*cursor == '\\' && cursor + 1 < end) ++cursor;
            destination[written++] = *cursor++;
        }
        destination[written] = '\0';
        if (*cursor == '\"') {
            const char *next_definition = strstr(cursor, "\"name\":\"");
            const char *definition_end = next_definition != NULL && next_definition < end ?
                                         next_definition : end;
            recover_raw_geometry(&parsed[index], definition, definition_end);
            char quoted_name[MOONRAKER_EXCLUDE_OBJECT_NAME_MAX_LEN + 3];
            snprintf(quoted_name, sizeof(quoted_name), "\"%s\"", destination);
            const char *excluded_match = excluded == NULL ? NULL : strstr(excluded, quoted_name);
            const char *current_match = current == NULL ? NULL : strstr(current, quoted_name);
            parsed[index].excluded = excluded_match != NULL && current != NULL &&
                                     excluded_match < current;
            parsed[index].current = current_match != NULL;
            ++index;
        }
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (exclude_objects == NULL) {
        xSemaphoreGive(state_lock);
        free(parsed);
        return false;
    }
    free(exclude_objects->objects);
    exclude_objects->objects = parsed;
    exclude_objects->count = index;
    exclude_objects->truncated = false;
    exclude_objects->loading = false;
    exclude_objects->valid = true;
    exclude_objects->unavailable = false;
    exclude_objects->error[0] = '\0';
    ++exclude_objects->generation;
    xSemaphoreGive(state_lock);
    exclude_objects_request_id = -1;
    ESP_LOGI(TAG, "Recovered %u exclude objects with compact geometry", (unsigned)index);
    return true;
}

static void apply_object_status(cJSON *objects)
{
    if (!cJSON_IsObject(objects)) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cJSON *print_stats = object_item(objects, "print_stats");
    if (print_stats != NULL) {
        cJSON *print_state = cJSON_GetObjectItemCaseSensitive(print_stats, "state");
        cJSON *print_message = cJSON_GetObjectItemCaseSensitive(print_stats, "message");
        cJSON *filename = cJSON_GetObjectItemCaseSensitive(print_stats, "filename");
        if (cJSON_IsString(print_state)) {
            cached_printer.job_state = parse_job_state(print_state->valuestring);
            if (cached_printer.job_state != PRINTER_JOB_ERROR) cached_printer.job_message[0] = '\0';
        }
        if (cJSON_IsString(print_message)) {
            strlcpy(cached_printer.job_message, print_message->valuestring,
                    sizeof(cached_printer.job_message));
        }
        if (cJSON_IsString(filename)) {
            strlcpy(cached_printer.filename, filename->valuestring, sizeof(cached_printer.filename));
        }
        update_print_duration(print_stats);
    }
    cJSON *webhooks = object_item(objects, "webhooks");
    if (webhooks != NULL) {
        cJSON *webhooks_state = cJSON_GetObjectItemCaseSensitive(webhooks, "state");
        cJSON *state_message = cJSON_GetObjectItemCaseSensitive(webhooks, "state_message");
        if (cJSON_IsString(webhooks_state)) {
            apply_klippy_state_locked(webhooks_state->valuestring,
                                      cJSON_IsString(state_message) ? state_message->valuestring : NULL);
        } else if (cJSON_IsString(state_message)) {
            strlcpy(cached_printer.state_message, state_message->valuestring,
                    sizeof(cached_printer.state_message));
        }
    }
    cJSON *extruder = object_item(objects, "extruder");
    if (extruder != NULL) update_temperature(extruder, &cached_printer.hotend_current_deci_c,
                                               &cached_printer.hotend_target_deci_c);
    cJSON *bed = object_item(objects, "heater_bed");
    if (bed != NULL) update_temperature(bed, &cached_printer.bed_current_deci_c,
                                          &cached_printer.bed_target_deci_c);
    cJSON *virtual_sdcard = object_item(objects, "virtual_sdcard");
    if (virtual_sdcard != NULL) update_progress(virtual_sdcard);
    cJSON *toolhead = object_item(objects, "toolhead");
    if (toolhead != NULL) update_toolhead_position(toolhead);
    cJSON *manual = object_item(objects, "manual_probe");
    if (manual != NULL) {
        cJSON *active = cJSON_GetObjectItemCaseSensitive(manual, "is_active");
        cJSON *position = cJSON_GetObjectItemCaseSensitive(manual, "z_position");
        cJSON *lower = cJSON_GetObjectItemCaseSensitive(manual, "z_position_lower");
        cJSON *upper = cJSON_GetObjectItemCaseSensitive(manual, "z_position_upper");
        if (cJSON_IsBool(active)) manual_probe.is_active = cJSON_IsTrue(active);
        if (position != NULL) {
            manual_probe.position_valid = cJSON_IsNumber(position);
            if (manual_probe.position_valid) manual_probe.z_position = (float)position->valuedouble;
        }
        if (lower != NULL) {
            manual_probe.lower_valid = cJSON_IsNumber(lower);
            if (manual_probe.lower_valid) manual_probe.z_position_lower = (float)lower->valuedouble;
        }
        if (upper != NULL) {
            manual_probe.upper_valid = cJSON_IsNumber(upper);
            if (manual_probe.upper_valid) manual_probe.z_position_upper = (float)upper->valuedouble;
        }
        ++manual_probe.generation;
    }
    update_tune_values(object_item(objects, "gcode_move"), object_item(objects, "fan"));
    update_exclude_objects_locked(object_item(objects, "exclude_object"));
    client_status.last_update_ms = now_ms();
    xSemaphoreGive(state_lock);
}

static int send_rpc(const char *method, const char *params)
{
    if (!socket_connected || websocket == NULL) return -1;
    char stack_request[512];
    int id = ++request_id;
    const char *resolved_params = params == NULL ? "{}" : params;
    int length = snprintf(NULL, 0,
                          "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
                          method, resolved_params, id);
    if (length <= 0 || length > (int)MAX_MESSAGE_SIZE) {
        ESP_LOGE(TAG, "RPC %s has invalid size %d", method, length);
        return -1;
    }
    char *request = stack_request;
    if ((size_t)length >= sizeof(stack_request)) {
        /* Most control RPCs stay on the stack.  Large subscriptions and
         * scripts borrow PSRAM only for the duration of the synchronous send. */
        request = heap_caps_malloc((size_t)length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (request == NULL) {
            ESP_LOGE(TAG, "Not enough PSRAM for %d-byte RPC %s", length, method);
            return -1;
        }
    }
    int rendered = snprintf(request, (size_t)length + 1,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
                            method, resolved_params, id);
    if (rendered != length) {
        if (request != stack_request) heap_caps_free(request);
        ESP_LOGE(TAG, "Could not render RPC %s", method);
        return -1;
    }
    int sent = esp_websocket_client_send_text(websocket, request, length, pdMS_TO_TICKS(1000));
    if (request != stack_request) heap_caps_free(request);
    if (sent < 0) {
        ESP_LOGW(TAG, "failed to send RPC %s", method);
        return -1;
    }
    ESP_LOGI(TAG, "RPC %s id=%d", method, id);
    return id;
}

static bool json_quote(const char *source, char *dest, size_t dest_size)
{
    if (source == NULL || dest == NULL || dest_size < 3) return false;
    size_t out = 0;
    dest[out++] = '\"';
    for (const unsigned char *p = (const unsigned char *)source; *p != '\0'; ++p) {
        if (*p == '\"' || *p == '\\') {
            if (out + 2 >= dest_size) return false;
            dest[out++] = '\\';
            dest[out++] = (char)*p;
            continue;
        }
        /* G-code scripts consist of newline-separated commands. JSON permits
         * those control characters only when escaped, so rejecting them here
         * made every multi-line script fail before it reached Moonraker. */
        if (*p == '\b') {
            if (out + 2 >= dest_size) return false;
            dest[out++] = '\\';
            dest[out++] = 'b';
            continue;
        }
        if (*p == '\f') {
            if (out + 2 >= dest_size) return false;
            dest[out++] = '\\';
            dest[out++] = 'f';
            continue;
        }
        if (*p == '\n') {
            if (out + 2 >= dest_size) return false;
            dest[out++] = '\\';
            dest[out++] = 'n';
            continue;
        }
        if (*p == '\r') {
            if (out + 2 >= dest_size) return false;
            dest[out++] = '\\';
            dest[out++] = 'r';
            continue;
        }
        if (*p == '\t') {
            if (out + 2 >= dest_size) return false;
            dest[out++] = '\\';
            dest[out++] = 't';
            continue;
        }
        if (*p < 0x20) {
            if (out + 6 >= dest_size) return false;
            const int written = snprintf(dest + out, dest_size - out, "\\u%04x", *p);
            if (written != 6) return false;
            out += 6;
            continue;
        }
        if (out + 2 >= dest_size) return false;
        dest[out++] = (char)*p;
    }
    dest[out++] = '\"';
    dest[out] = '\0';
    return true;
}

static void apply_directory_result(cJSON *result)
{
    if (!cJSON_IsObject(result)) return;
    file_browser_list_entry_t *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    cJSON *dirs = cJSON_GetObjectItemCaseSensitive(result, "dirs");
    cJSON *files = cJSON_GetObjectItemCaseSensitive(result, "files");
    const cJSON *collections[] = { dirs, files };
    for (size_t group = 0; group < 2; ++group) {
        if (!cJSON_IsArray(collections[group])) continue;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, collections[group]) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, group == 0 ? "dirname" : "filename");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
            cJSON *modified = cJSON_GetObjectItemCaseSensitive(item, "modified");
            if (!cJSON_IsString(name)) continue;
            /* Moonraker service directories such as .thumbs contain assets,
             * not printable files, and can produce listings larger than the
             * pendant's bounded websocket message buffer. */
            if (group == 0 && name->valuestring[0] == '.') continue;
            if (group == 1) {
                const char *dot = strrchr(name->valuestring, '.');
                if (dot == NULL || (strcasecmp(dot, ".gcode") != 0 && strcasecmp(dot, ".gco") != 0)) continue;
            }
            if (entry_count == entry_capacity) {
                const size_t new_capacity = entry_capacity == 0 ? 16 : entry_capacity * 2;
                file_browser_list_entry_t *grown = realloc(entries, new_capacity * sizeof(*entries));
                if (grown == NULL) {
                    free(entries);
                    xSemaphoreTake(state_lock, portMAX_DELAY);
                    file_browser.loading = false;
                    file_browser.valid = false;
                    strlcpy(file_browser.error, "Not enough memory for file list", sizeof(file_browser.error));
                    ++file_browser.generation;
                    xSemaphoreGive(state_lock);
                    return;
                }
                entries = grown;
                entry_capacity = new_capacity;
            }
            file_browser_list_entry_t *entry = &entries[entry_count++];
            memset(entry, 0, sizeof(*entry));
            strlcpy(entry->name, name->valuestring, sizeof(entry->name));
            entry->is_directory = group == 0;
            entry->size_bytes = cJSON_IsNumber(size) && size->valuedouble > 0 ?
                                (uint32_t)size->valuedouble : 0;
            entry->modified_ms = cJSON_IsNumber(modified) && modified->valuedouble > 0 ?
                                 (uint64_t)(modified->valuedouble * 1000.0) : 0;
        }
    }
    if (entry_count > 1) qsort(entries, entry_count, sizeof(*entries), compare_file_entries);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    free(file_browser_entries);
    file_browser_entries = entries;
    file_browser.entry_count = entry_count;
    file_browser.loading = false;
    file_browser.valid = true;
    file_browser.error[0] = '\0';
    ++file_browser.generation;
    xSemaphoreGive(state_lock);
}

static void apply_disk_info_result(cJSON *result)
{
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(result, "disk_usage");
    cJSON *total = usage == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(usage, "total");
    cJSON *used = usage == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(usage, "used");
    cJSON *free_space = usage == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(usage, "free");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    disk_info.loading = false;
    disk_info.valid = cJSON_IsNumber(total) && cJSON_IsNumber(used) && cJSON_IsNumber(free_space);
    if (disk_info.valid) {
        disk_info.total_bytes = (uint64_t)total->valuedouble;
        disk_info.used_bytes = (uint64_t)used->valuedouble;
        disk_info.free_bytes = (uint64_t)free_space->valuedouble;
        disk_info.error[0] = '\0';
    } else {
        strlcpy(disk_info.error, "Disk information unavailable", sizeof(disk_info.error));
    }
    ++disk_info.generation;
    xSemaphoreGive(state_lock);
}

static void apply_metadata_result(cJSON *result)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    moonraker_file_entry_t *entry = &file_metadata.file;
    cJSON *estimated_time = cJSON_GetObjectItemCaseSensitive(result, "estimated_time");
    cJSON *filament_weight = cJSON_GetObjectItemCaseSensitive(result, "filament_weight_total");
    cJSON *filament_type = cJSON_GetObjectItemCaseSensitive(result, "filament_type");
    cJSON *filament_name = cJSON_GetObjectItemCaseSensitive(result, "filament_name");
    cJSON *thumbnails = cJSON_GetObjectItemCaseSensitive(result, "thumbnails");
    entry->estimated_time_seconds = cJSON_IsNumber(estimated_time) && estimated_time->valuedouble > 0 ?
                                    (uint32_t)estimated_time->valuedouble : 0;
    entry->filament_weight_deci_g = cJSON_IsNumber(filament_weight) && filament_weight->valuedouble > 0 ?
                                    (uint16_t)(filament_weight->valuedouble * 10.0) : 0;
    if (cJSON_IsString(filament_type)) strlcpy(entry->filament_type, filament_type->valuestring, sizeof(entry->filament_type));
    if (cJSON_IsString(filament_name)) strlcpy(entry->filament_name, filament_name->valuestring, sizeof(entry->filament_name));
    entry->has_thumbnail = cJSON_IsArray(thumbnails) && cJSON_GetArraySize(thumbnails) > 0;
    file_metadata.loading = false;
    file_metadata.valid = true;
    file_metadata.error[0] = '\0';
    ++file_metadata.generation;
    xSemaphoreGive(state_lock);
}

static void apply_macros_result(cJSON *result)
{
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(result, "objects");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    macros.count = 0;
    if (cJSON_IsArray(objects)) {
        cJSON *object = NULL;
        cJSON_ArrayForEach(object, objects) {
            if (!cJSON_IsString(object) || macros.count >= MOONRAKER_MAX_MACROS) continue;
            static const char prefix[] = "gcode_macro ";
            if (strncmp(object->valuestring, prefix, sizeof(prefix) - 1) != 0) continue;
            strlcpy(macros.names[macros.count++], object->valuestring + sizeof(prefix) - 1,
                    sizeof(macros.names[0]));
        }
    }
    macros.loading = false;
    macros.valid = true;
    macros.error[0] = '\0';
    ++macros.generation;
    xSemaphoreGive(state_lock);
}

static void apply_history_result(cJSON *result)
{
    cJSON *jobs = cJSON_GetObjectItemCaseSensitive(result, "jobs");
    cJSON *total = cJSON_GetObjectItemCaseSensitive(result, "total");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    history.count = 0;
    if (cJSON_IsArray(jobs)) {
        cJSON *job = NULL;
        cJSON_ArrayForEach(job, jobs) {
            if (!cJSON_IsObject(job) || history.count >= MOONRAKER_HISTORY_PAGE_SIZE) continue;
            moonraker_history_job_t *entry = &history_jobs[history.count++];
            memset(entry, 0, sizeof(*entry));
            cJSON *value = cJSON_GetObjectItemCaseSensitive(job, "job_id");
            if (cJSON_IsString(value)) strlcpy(entry->id, value->valuestring, sizeof(entry->id));
            value = cJSON_GetObjectItemCaseSensitive(job, "filename");
            if (cJSON_IsString(value)) strlcpy(entry->filename, value->valuestring, sizeof(entry->filename));
            value = cJSON_GetObjectItemCaseSensitive(job, "status");
            if (cJSON_IsString(value)) strlcpy(entry->status, value->valuestring, sizeof(entry->status));
            value = cJSON_GetObjectItemCaseSensitive(job, "message");
            if (cJSON_IsString(value)) strlcpy(entry->message, value->valuestring, sizeof(entry->message));
            value = cJSON_GetObjectItemCaseSensitive(job, "start_time");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) entry->start_time_ms = (uint64_t)(value->valuedouble * 1000.0);
            value = cJSON_GetObjectItemCaseSensitive(job, "end_time");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) entry->end_time_ms = (uint64_t)(value->valuedouble * 1000.0);
            value = cJSON_GetObjectItemCaseSensitive(job, "total_duration");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) entry->duration_seconds = (uint32_t)value->valuedouble;
            value = cJSON_GetObjectItemCaseSensitive(job, "filament_used");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) entry->filament_used_mm = (uint32_t)value->valuedouble;
            value = cJSON_GetObjectItemCaseSensitive(job, "exists");
            entry->file_exists = cJSON_IsTrue(value);
        }
    }
    /* Current Moonraker versions return only `count` with list results.
     * Keep support for older extensions that provide `total`, otherwise
     * retain the value obtained from server.history.totals. */
    if (cJSON_IsNumber(total) && total->valueint >= 0) {
        history.total = (size_t)total->valueint;
    } else if (history.total < history.offset + history.count) {
        history.total = history.offset + history.count;
    }
    history.loading = false;
    history.valid = true;
    history.unavailable = false;
    history.error[0] = '\0';
    ++history.generation;
    xSemaphoreGive(state_lock);
}

static void apply_history_totals_result(cJSON *result)
{
    cJSON *job_totals = cJSON_GetObjectItemCaseSensitive(result, "job_totals");
    if (!cJSON_IsObject(job_totals)) return;
    cJSON *total_jobs = cJSON_GetObjectItemCaseSensitive(job_totals, "total_jobs");
    if (!cJSON_IsNumber(total_jobs) || total_jobs->valuedouble < 0) return;

    xSemaphoreTake(state_lock, portMAX_DELAY);
    history_totals.total_jobs = (uint64_t)total_jobs->valuedouble;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(job_totals, "total_time");
    history_totals.total_time_seconds = cJSON_IsNumber(value) && value->valuedouble > 0 ?
        (uint64_t)value->valuedouble : 0;
    value = cJSON_GetObjectItemCaseSensitive(job_totals, "total_print_time");
    history_totals.total_print_time_seconds = cJSON_IsNumber(value) && value->valuedouble > 0 ?
        (uint64_t)value->valuedouble : 0;
    value = cJSON_GetObjectItemCaseSensitive(job_totals, "total_filament_used");
    history_totals.total_filament_used_mm = cJSON_IsNumber(value) && value->valuedouble > 0 ?
        (uint64_t)value->valuedouble : 0;
    value = cJSON_GetObjectItemCaseSensitive(job_totals, "longest_print");
    history_totals.longest_print_seconds = cJSON_IsNumber(value) && value->valuedouble > 0 ?
        (uint64_t)value->valuedouble : 0;
    history_totals.loading = false;
    history_totals.valid = true;
    history_totals.error[0] = '\0';
    ++history_totals.generation;

    history.total = (size_t)history_totals.total_jobs;
    if (history.total < history.offset + history.count) {
        history.total = history.offset + history.count;
    }
    ++history.generation;
    xSemaphoreGive(state_lock);
}

static void send_server_info(void)
{
    server_info_request_id = send_rpc("server.info", "{}");
    if (server_info_request_id >= 0) {
        request_started_ms = now_ms();
        set_state(MOONRAKER_KLIPPY_WAIT, "Waiting for Klippy");
    }
}

static void send_subscription(void)
{
    static const char *params =
        "{\"objects\":{\"webhooks\":[\"state\",\"state_message\"],"
        "\"print_stats\":[\"state\",\"message\",\"filename\",\"print_duration\"],"
        "\"virtual_sdcard\":[\"progress\"],\"display_status\":[\"progress\"],"
        "\"extruder\":[\"temperature\",\"target\"],"
        "\"heater_bed\":[\"temperature\",\"target\"],"
        "\"gcode_move\":[\"speed_factor\",\"extrude_factor\",\"homing_origin\"],"
        "\"fan\":[\"speed\"],"
        "\"toolhead\":[\"position\",\"homed_axes\"],"
        "\"manual_probe\":[\"is_active\",\"z_position\",\"z_position_lower\",\"z_position_upper\"],"
        "\"exclude_object\":[\"excluded_objects\",\"current_object\"]}}";
    subscription_request_id = send_rpc("printer.objects.subscribe", params);
    if (subscription_request_id >= 0) {
        request_started_ms = now_ms();
        set_state(MOONRAKER_SUBSCRIBING, "Subscribing to printer status");
    }
}

static void request_klippy_state(void)
{
    /* A firmware restart invalidates Moonraker subscriptions.  Re-check the
     * server state before subscribing again, even if the websocket survived. */
    identify_pending = false;
    subscription_pending = false;
    server_info_pending = true;
    server_info_request_id = -1;
    subscription_request_id = -1;
}

static void handle_response(cJSON *message)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(message, "error");
    if (cJSON_IsObject(error)) {
        cJSON *message_text = cJSON_GetObjectItemCaseSensitive(error, "message");
        if (cJSON_IsNumber(id) && id->valueint == gcode_request_id) {
            gcode_request_id = -1;
            /* A motion can be rejected before homing or while the printer is
             * busy. This must not tear down the live status subscription. */
            ESP_LOGW(TAG, "G-code rejected: %s",
                     cJSON_IsString(message_text) ? message_text->valuestring : "unknown error");
            console_append("! ", cJSON_IsString(message_text) ? message_text->valuestring : "G-code rejected");
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == gcode_store_request_id) {
            gcode_store_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            console.loading = false;
            strlcpy(console.error, cJSON_IsString(message_text) ? message_text->valuestring :
                    "Console history unavailable", sizeof(console.error));
            ++console.generation;
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == directory_request_id) {
            directory_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            file_browser.loading = false;
            strlcpy(file_browser.error, cJSON_IsString(message_text) ? message_text->valuestring : "Could not load folder",
                    sizeof(file_browser.error));
            ++file_browser.generation;
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == metadata_request_id) {
            metadata_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            file_metadata.loading = false;
            strlcpy(file_metadata.error, cJSON_IsString(message_text) ? message_text->valuestring : "Metadata unavailable",
                    sizeof(file_metadata.error));
            ++file_metadata.generation;
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == macros_request_id) {
            macros_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            macros.loading = false;
            strlcpy(macros.error, cJSON_IsString(message_text) ? message_text->valuestring : "Could not load macros",
                    sizeof(macros.error));
            ++macros.generation;
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == disk_info_request_id) {
            disk_info_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            disk_info.loading = false;
            disk_info.valid = false;
            strlcpy(disk_info.error, cJSON_IsString(message_text) ? message_text->valuestring :
                    "Disk information unavailable", sizeof(disk_info.error));
            ++disk_info.generation;
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == history_request_id) {
            history_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            history.loading = false;
            history.unavailable = true;
            strlcpy(history.error, "History unavailable", sizeof(history.error));
            ++history.generation;
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == history_totals_request_id) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            history_totals.loading = false;
            strlcpy(history_totals.error, cJSON_IsString(message_text) ? message_text->valuestring :
                    "Statistics unavailable", sizeof(history_totals.error));
            ++history_totals.generation;
            xSemaphoreGive(state_lock);
            /* A list response is still usable if an older Moonraker lacks
             * totals; it simply cannot expose a next-page arrow. */
            history_totals_request_id = -1;
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == subscription_request_id) {
            subscription_request_id = -1;
            set_klippy_state(NULL, NULL);
            set_state(MOONRAKER_KLIPPY_WAIT,
                      cJSON_IsString(message_text) ? message_text->valuestring : "Klippy not ready");
            request_klippy_state();
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == exclude_objects_request_id) {
            exclude_objects_request_id = -1;
            xSemaphoreTake(state_lock, portMAX_DELAY);
            if (exclude_objects != NULL) {
                exclude_objects->loading = false; exclude_objects->valid = false;
                exclude_objects->unavailable = true;
                strlcpy(exclude_objects->error, "Object exclusion unavailable", sizeof(exclude_objects->error));
                ++exclude_objects->generation;
            }
            xSemaphoreGive(state_lock);
            return;
        }
        if (cJSON_IsNumber(id) && id->valueint == ignored_exclude_objects_request_id) {
            ignored_exclude_objects_request_id = -1;
            return;
        }
        set_state(MOONRAKER_PROTOCOL_ERROR,
                  cJSON_IsString(message_text) ? message_text->valuestring : "Moonraker JSON-RPC error");
        socket_closed = true;
        return;
    }
    cJSON *result = cJSON_GetObjectItemCaseSensitive(message, "result");
    if (!cJSON_IsNumber(id)) return;
    if (id->valueint == gcode_request_id) {
        gcode_request_id = -1;
        console_append("< ", "Command accepted");
        return;
    }
    if (id->valueint == ignored_exclude_objects_request_id) {
        ignored_exclude_objects_request_id = -1;
        return;
    }
    if (!cJSON_IsObject(result)) return;
    ESP_LOGI(TAG, "RPC response id=%d", id->valueint);
    if (id->valueint == identify_request_id) {
        identify_request_id = -1;
        server_info_pending = true;
    } else if (id->valueint == server_info_request_id) {
        server_info_request_id = -1;
        cJSON *klippy_state = cJSON_GetObjectItemCaseSensitive(result, "klippy_state");
        cJSON *message_text = cJSON_GetObjectItemCaseSensitive(result, "klippy_state_message");
        if (cJSON_IsString(klippy_state) && strcmp(klippy_state->valuestring, "ready") == 0) {
            set_klippy_state("ready", cJSON_IsString(message_text) ? message_text->valuestring : NULL);
            subscription_pending = true;
        } else {
            const char *detail = cJSON_IsString(message_text) ? message_text->valuestring : "Klippy not ready";
            if (cJSON_IsString(klippy_state)) set_klippy_state(klippy_state->valuestring, detail);
            if (cJSON_IsString(klippy_state) && strcmp(klippy_state->valuestring, "error") == 0) {
                set_state(MOONRAKER_KLIPPY_ERROR, detail);
            } else if (cJSON_IsString(klippy_state) && strcmp(klippy_state->valuestring, "shutdown") == 0) {
                set_state(MOONRAKER_KLIPPY_ERROR, detail);
            } else {
                set_state(MOONRAKER_KLIPPY_WAIT, detail);
            }
        }
    } else if (id->valueint == subscription_request_id) {
        subscription_request_id = -1;
        cJSON *status = object_item(result, "status");
        if (status != NULL) {
            apply_object_status(status);
            set_state(MOONRAKER_ONLINE, "Live status subscription");
        } else {
            set_state(MOONRAKER_PROTOCOL_ERROR, "Subscription returned no printer status");
            socket_closed = true;
        }
    } else if (id->valueint == directory_request_id) {
        directory_request_id = -1;
        apply_directory_result(result);
    } else if (id->valueint == metadata_request_id) {
        metadata_request_id = -1;
        apply_metadata_result(result);
    } else if (id->valueint == macros_request_id) {
        macros_request_id = -1;
        apply_macros_result(result);
    } else if (id->valueint == disk_info_request_id) {
        disk_info_request_id = -1;
        apply_disk_info_result(result);
    } else if (id->valueint == history_request_id) {
        history_request_id = -1;
        apply_history_result(result);
    } else if (id->valueint == history_totals_request_id) {
        history_totals_request_id = -1;
        apply_history_totals_result(result);
    } else if (id->valueint == gcode_store_request_id) {
        gcode_store_request_id = -1;
        apply_gcode_store_result(result);
    } else if (id->valueint == exclude_objects_request_id) {
        exclude_objects_request_id = -1;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        update_exclude_objects_locked(object_item(result, "status"));
        xSemaphoreGive(state_lock);
    }
}

static void handle_message(const char *data, int length)
{
    char *json_text = calloc(1, (size_t)length + 1);
    if (json_text == NULL) return;
    memcpy(json_text, data, (size_t)length);
    if (recover_exclude_objects(json_text)) {
        free(json_text);
        return;
    }
    cJSON *message = cJSON_Parse(json_text);
    free(json_text);
    if (message == NULL) {
        set_state(MOONRAKER_PROTOCOL_ERROR, "Invalid JSON from Moonraker");
        socket_closed = true;
        return;
    }
    cJSON *method = cJSON_GetObjectItemCaseSensitive(message, "method");
    if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_status_update") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(message, "params");
        cJSON *objects = cJSON_IsArray(params) ? cJSON_GetArrayItem(params, 0) : NULL;
        apply_object_status(objects);
    } else if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_gcode_response") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(message, "params");
        cJSON *response = cJSON_IsArray(params) ? cJSON_GetArrayItem(params, 0) : NULL;
        if (cJSON_IsString(response)) {
            apply_prompt_response(response->valuestring);
            console_append("< ", response->valuestring);
        }
    } else if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_klippy_disconnected") == 0) {
        set_klippy_state(NULL, NULL);
        set_state(MOONRAKER_KLIPPY_WAIT, "Klippy disconnected");
        request_klippy_state();
    } else if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_klippy_ready") == 0) {
        set_klippy_state("ready", NULL);
        request_klippy_state();
    } else if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_klippy_shutdown") == 0) {
        set_klippy_state("shutdown", "Klipper shutdown");
        set_state(MOONRAKER_KLIPPY_ERROR, "Klipper shutdown");
        request_klippy_state();
    } else if (cJSON_IsString(method) && strcmp(method->valuestring, "notify_filelist_changed") == 0) {
        file_list_changed = true;
    } else {
        handle_response(message);
    }
    cJSON_Delete(message);
}

static void reset_message_buffer(void)
{
    free(message_buffer);
    message_buffer = NULL;
    message_size = 0;
}

static void handle_websocket_data(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->data_len < 0 || data->payload_len <= 0 || data->payload_offset < 0) return;
    const uint8_t opcode = data->op_code & 0x0f;
    if (opcode != WS_TRANSPORT_OPCODES_TEXT && opcode != WS_TRANSPORT_OPCODES_CONT) return;
    if ((size_t)data->payload_len > MAX_MESSAGE_SIZE ||
        (size_t)data->payload_offset + (size_t)data->data_len > (size_t)data->payload_len) {
        reset_message_buffer();
        set_state(MOONRAKER_PROTOCOL_ERROR, "Moonraker message is too large or malformed");
        socket_closed = true;
        return;
    }
    if (data->payload_offset == 0) {
        reset_message_buffer();
        message_buffer = calloc(1, (size_t)data->payload_len + 1);
        if (message_buffer == NULL) {
            set_state(MOONRAKER_PROTOCOL_ERROR, "Out of memory receiving Moonraker message");
            socket_closed = true;
            return;
        }
        message_size = (size_t)data->payload_len;
    }
    if (message_buffer == NULL || message_size != (size_t)data->payload_len) {
        reset_message_buffer();
        set_state(MOONRAKER_PROTOCOL_ERROR, "Incomplete Moonraker message");
        socket_closed = true;
        return;
    }
    memcpy(message_buffer + data->payload_offset, data->data_ptr, (size_t)data->data_len);
    if ((size_t)data->payload_offset + (size_t)data->data_len == message_size) {
        handle_message(message_buffer, (int)message_size);
        reset_message_buffer();
    }
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WebSocket connected");
        socket_connected = true;
        socket_closed = false;
        set_connection(PRINTER_CONNECTION_ONLINE);
        set_state(MOONRAKER_IDENTIFYING, "Identifying display client");
        identify_pending = true;
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        handle_websocket_data(data);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_ERROR ||
               event_id == WEBSOCKET_EVENT_CLOSED) {
        ESP_LOGW(TAG, "WebSocket event: %" PRIi32, event_id);
        if (data != NULL) {
            ESP_LOGW(TAG, "websocket error type=%d http=%d errno=%d", data->error_handle.error_type,
                     data->error_handle.esp_ws_handshake_status_code, data->error_handle.esp_transport_sock_errno);
        }
        socket_connected = false;
        socket_closed = true;
        set_connection(PRINTER_CONNECTION_OFFLINE);
    }
}

static bool build_uri(const settings_printer_t *config, char *uri, size_t uri_size)
{
    const char *host = config->host;
    if (strncmp(host, "http://", 7) == 0) host += 7;
    else if (strncmp(host, "ws://", 5) == 0) host += 5;
    else if (strstr(host, "://") != NULL) return false;
    char endpoint[SETTINGS_PRINTER_HOST_MAX_LEN + 1];
    size_t length = strcspn(host, "/?#");
    if (length == 0 || length >= sizeof(endpoint)) return false;
    memcpy(endpoint, host, length);
    endpoint[length] = '\0';
    int result = snprintf(uri, uri_size, strchr(endpoint, ':') == NULL ? "ws://%s:%u/websocket" : "ws://%s/websocket",
                          endpoint, config->port);
    return result > 0 && (size_t)result < uri_size;
}

static bool build_emergency_stop_uri(const settings_printer_t *config, char *uri, size_t uri_size)
{
    char websocket_uri[sizeof(active_uri)];
    if (!build_uri(config, websocket_uri, sizeof(websocket_uri))) return false;
    const char *endpoint = websocket_uri + strlen("ws://");
    const char *path = strstr(endpoint, "/websocket");
    if (path == NULL) return false;
    int result = snprintf(uri, uri_size, "http://%.*s/printer/emergency_stop",
                          (int)(path - endpoint), endpoint);
    return result > 0 && (size_t)result < uri_size;
}

static bool active_configuration_matches(const settings_printer_t *config)
{
    char uri[sizeof(active_uri)];
    return build_uri(config, uri, sizeof(uri)) && strcmp(uri, active_uri) == 0;
}

static void destroy_websocket(void)
{
    if (websocket != NULL) {
        if (esp_websocket_client_is_connected(websocket)) esp_websocket_client_close(websocket, pdMS_TO_TICKS(1000));
        esp_websocket_client_stop(websocket);
        esp_websocket_client_destroy(websocket);
        websocket = NULL;
    }
    reset_message_buffer();
    socket_connected = false;
    socket_closed = false;
    identify_pending = false;
    server_info_pending = false;
    subscription_pending = false;
    identify_request_id = -1;
    server_info_request_id = -1;
    subscription_request_id = -1;
    directory_request_id = -1;
    metadata_request_id = -1;
    macros_request_id = -1;
    history_request_id = -1;
    history_totals_request_id = -1;
    gcode_request_id = -1;
    gcode_store_request_id = -1;
    active_uri[0] = '\0';
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (file_browser.loading) {
        file_browser.loading = false;
        strlcpy(file_browser.error, "Connection lost", sizeof(file_browser.error));
        ++file_browser.generation;
    }
    if (file_metadata.loading) {
        file_metadata.loading = false;
        strlcpy(file_metadata.error, "Connection lost", sizeof(file_metadata.error));
        ++file_metadata.generation;
    }
    if (macros.loading) {
        macros.loading = false;
        strlcpy(macros.error, "Connection lost", sizeof(macros.error));
        ++macros.generation;
    }
    if (history.loading) {
        history.loading = false;
        strlcpy(history.error, "Connection lost", sizeof(history.error));
        ++history.generation;
    }
    if (history_totals.loading) {
        history_totals.loading = false;
        strlcpy(history_totals.error, "Connection lost", sizeof(history_totals.error));
        ++history_totals.generation;
    }
    free(history_jobs);
    history_jobs = NULL;
    history.count = 0;
    history.valid = false;
    /* Totals belong to the disconnected printer.  Do not show them while a
     * new printer connection is being established. */
    history_totals.valid = false;
    ++history_totals.generation;
    manual_probe.is_active = false;
    manual_probe.position_valid = false;
    manual_probe.lower_valid = false;
    manual_probe.upper_valid = false;
    ++manual_probe.generation;
    prompt.visible = false;
    ++prompt.generation;
    xSemaphoreGive(state_lock);
}

static void moonraker_task(void *context)
{
    (void)context;
    uint32_t next_attempt = 0;
    uint32_t backoff_ms = 1000;
    uint32_t last_server_info = 0;
    uint32_t last_reconcile = 0;
    bool was_suspended = false;
    for (;;) {
        if (moonraker_suspended) {
            if (!was_suspended) {
                destroy_websocket();
                set_connection(PRINTER_CONNECTION_OFFLINE);
                set_state(MOONRAKER_DISABLED, "Paused for firmware update");
                moonraker_suspend_complete = true;
                was_suspended = true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (was_suspended) {
            /* Do not retain a prior reconnect backoff after OTA releases the
             * network.  Reconnect normally as soon as the user leaves it. */
            next_attempt = 0;
            backoff_ms = 1000;
            was_suspended = false;
        }
        settings_printer_t config;
        wifi_manager_status_t wifi;
        settings_get_printer(&config);
        wifi_manager_get_status(&wifi);
        if (!settings_printer_is_valid(&config) || !config.enabled) {
            destroy_websocket();
            set_connection(PRINTER_CONNECTION_UNCONFIGURED);
            set_state(MOONRAKER_DISABLED, "Printer is not configured");
        } else if (wifi.state != WIFI_MANAGER_CONNECTED || wifi.ip[0] == '\0') {
            destroy_websocket();
            set_connection(PRINTER_CONNECTION_OFFLINE);
            set_state(MOONRAKER_WIFI_WAIT, "Waiting for Wi-Fi address");
        } else if (websocket != NULL && !active_configuration_matches(&config)) {
            destroy_websocket();
            set_connection(PRINTER_CONNECTION_OFFLINE);
            set_state(MOONRAKER_BACKOFF, "Printer configuration changed; reconnecting");
            next_attempt = now_ms();
            backoff_ms = 1000;
        } else if (websocket == NULL && deadline_reached(now_ms(), next_attempt)) {
            char uri[128];
            if (!build_uri(&config, uri, sizeof(uri))) {
                set_connection(PRINTER_CONNECTION_OFFLINE);
                set_state(MOONRAKER_PROTOCOL_ERROR, "Invalid Moonraker address");
                next_attempt = now_ms() + 30000;
                set_reconnect_delay(30000);
            } else {
                ESP_LOGI(TAG, "connecting to %s", uri);
                esp_websocket_client_config_t ws_config = {
                    .uri = uri,
                    .network_timeout_ms = 8000,
                    .reconnect_timeout_ms = 0,
                    .disable_auto_reconnect = true,
                    .buffer_size = 4096,
                };
                websocket = esp_websocket_client_init(&ws_config);
                if (websocket != NULL) {
                    strlcpy(active_uri, uri, sizeof(active_uri));
                    esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
                    set_connection(PRINTER_CONNECTION_OFFLINE);
                    set_state(MOONRAKER_CONNECTING, "Opening Moonraker WebSocket");
                    set_reconnect_delay(0);
                    if (esp_websocket_client_start(websocket) == ESP_OK) {
                        connect_started_ms = now_ms();
                        identify_pending = false;
                        server_info_pending = false;
                        subscription_pending = false;
                        continue;
                    }
                    destroy_websocket();
                }
                set_state(MOONRAKER_BACKOFF, "WebSocket connection failed");
                next_attempt = now_ms() + backoff_ms;
                set_reconnect_delay(backoff_ms);
                backoff_ms = increase_backoff(backoff_ms);
            }
        } else if (websocket != NULL && socket_closed) {
            destroy_websocket();
            set_connection(PRINTER_CONNECTION_OFFLINE);
            set_state(MOONRAKER_BACKOFF, "Connection lost; retrying");
            next_attempt = now_ms() + backoff_ms;
            set_reconnect_delay(backoff_ms);
            backoff_ms = increase_backoff(backoff_ms);
        } else if (websocket != NULL) {
            uint32_t now = now_ms();
            xSemaphoreTake(state_lock, portMAX_DELAY);
            moonraker_connection_state_t state = client_status.state;
            uint32_t last_update = client_status.last_update_ms;
            xSemaphoreGive(state_lock);
            if (!socket_connected && now - connect_started_ms >= 10000) {
                ESP_LOGW(TAG, "WebSocket connect watchdog expired");
                socket_closed = true;
            } else if (socket_connected &&
                       (identify_request_id >= 0 || server_info_request_id >= 0 || subscription_request_id >= 0) &&
                       now - request_started_ms >= RPC_RESPONSE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Moonraker RPC watchdog expired");
                socket_closed = true;
            } else if (socket_connected && identify_pending) {
                identify_pending = false;
                identify_request_id = send_rpc("server.connection.identify",
                    "{\"client_name\":\"Klipper Pendant\",\"version\":\"0.1\","
                    "\"type\":\"display\",\"url\":\"https://github.com/ibvadim/klipper-pendant\"}");
                if (identify_request_id < 0) socket_closed = true;
                else request_started_ms = now;
            } else if (socket_connected && server_info_pending) {
                server_info_pending = false;
                send_server_info();
                last_server_info = now;
                if (server_info_request_id < 0) socket_closed = true;
            } else if (socket_connected && subscription_pending) {
                subscription_pending = false;
                send_subscription();
                if (subscription_request_id < 0) socket_closed = true;
            } else if ((state == MOONRAKER_KLIPPY_WAIT || state == MOONRAKER_KLIPPY_ERROR) &&
                       server_info_request_id < 0 && now - last_server_info >= 2000) {
                server_info_pending = true;
                last_server_info = now;
            } else if (state == MOONRAKER_ONLINE) {
                backoff_ms = 1000;
                set_reconnect_delay(0);
                if (file_list_changed && directory_request_id < 0) {
                    char path[sizeof(file_browser.path)];
                    file_list_changed = false;
                    xSemaphoreTake(state_lock, portMAX_DELAY);
                    strlcpy(path, file_browser.path[0] ? file_browser.path : "gcodes", sizeof(path));
                    xSemaphoreGive(state_lock);
                    moonraker_request_directory(path);
                } else if (now - last_reconcile >= RECONCILE_MS || now - last_update >= STALE_UPDATE_MS) {
                    subscription_pending = true;
                    last_reconcile = now;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t moonraker_init(void)
{
    cJSON_Hooks json_hooks = {
        .malloc_fn = moonraker_json_malloc,
        .free_fn = moonraker_json_free,
    };
    cJSON_InitHooks(&json_hooks);
    state_lock = xSemaphoreCreateMutex();
    if (state_lock == NULL) return ESP_ERR_NO_MEM;
    printer_source_t source = { .get_state = moonraker_get_printer_state, .context = NULL };
    ESP_RETURN_ON_ERROR(printer_set_source(&source), TAG, "set printer source");
    return xTaskCreate(moonraker_task, "moonraker", 6144, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void moonraker_set_suspended(bool suspended)
{
    moonraker_suspended = suspended;
    if (!suspended) moonraker_suspend_complete = false;
}

bool moonraker_is_suspended(void)
{
    return moonraker_suspended && moonraker_suspend_complete;
}

void moonraker_get_status(moonraker_status_t *status)
{
    if (status == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *status = client_status;
    xSemaphoreGive(state_lock);
}

void moonraker_get_file_browser(moonraker_file_browser_t *browser)
{
    if (browser == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *browser = file_browser;
    xSemaphoreGive(state_lock);
}

void moonraker_get_file_browser_page(size_t offset, moonraker_file_browser_page_t *page)
{
    if (page == NULL) return;
    memset(page, 0, sizeof(*page));
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (offset < file_browser.entry_count) {
        page->offset = offset;
        page->entry_count = file_browser.entry_count - offset;
        if (page->entry_count > MOONRAKER_FILE_BROWSER_PAGE_SIZE) {
            page->entry_count = MOONRAKER_FILE_BROWSER_PAGE_SIZE;
        }
        for (size_t i = 0; i < page->entry_count; ++i) {
            const file_browser_list_entry_t *source = &file_browser_entries[offset + i];
            moonraker_file_entry_t *destination = &page->entries[i];
            strlcpy(destination->name, source->name, sizeof(destination->name));
            destination->size_bytes = source->size_bytes;
            destination->modified_ms = source->modified_ms;
            destination->is_directory = source->is_directory;
        }
    }
    xSemaphoreGive(state_lock);
}

bool moonraker_get_file_browser_entry(size_t index, moonraker_file_entry_t *entry)
{
    if (entry == NULL) return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (index >= file_browser.entry_count || file_browser_entries == NULL) {
        xSemaphoreGive(state_lock);
        return false;
    }
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->name, file_browser_entries[index].name, sizeof(entry->name));
    entry->size_bytes = file_browser_entries[index].size_bytes;
    entry->modified_ms = file_browser_entries[index].modified_ms;
    entry->is_directory = file_browser_entries[index].is_directory;
    xSemaphoreGive(state_lock);
    return true;
}

void moonraker_get_file_metadata(moonraker_file_metadata_t *metadata)
{
    if (metadata == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *metadata = file_metadata;
    xSemaphoreGive(state_lock);
}

void moonraker_get_macros(moonraker_macro_list_t *list)
{
    if (list == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *list = macros;
    xSemaphoreGive(state_lock);
}

void moonraker_get_disk_info(moonraker_disk_info_t *info)
{
    if (info == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *info = disk_info;
    xSemaphoreGive(state_lock);
}

esp_err_t moonraker_request_disk_info(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (disk_info.loading) {
        xSemaphoreGive(state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    disk_info.loading = true;
    disk_info.valid = false;
    disk_info.error[0] = '\0';
    ++disk_info.generation;
    xSemaphoreGive(state_lock);

    /* Moonraker includes the filesystem capacity for the requested root in
     * every directory response.  This keeps the figure aligned with where
     * print files are actually stored, even when the host has several disks. */
    disk_info_request_id = send_rpc("server.files.get_directory",
                                    "{\"path\":\"gcodes\",\"extended\":false}");
    if (disk_info_request_id >= 0) return ESP_OK;

    xSemaphoreTake(state_lock, portMAX_DELAY);
    disk_info.loading = false;
    strlcpy(disk_info.error, "Moonraker unavailable", sizeof(disk_info.error));
    ++disk_info.generation;
    xSemaphoreGive(state_lock);
    return ESP_ERR_INVALID_STATE;
}

void moonraker_get_history_status(moonraker_history_status_t *status)
{
    if (status == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    status->offset = history.offset;
    status->count = history.count;
    status->total = history.total;
    status->generation = history.generation;
    status->loading = history.loading;
    status->valid = history.valid;
    status->unavailable = history.unavailable;
    strlcpy(status->error, history.error, sizeof(status->error));
    xSemaphoreGive(state_lock);
}

void moonraker_get_history_totals(moonraker_history_totals_t *totals)
{
    if (totals == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *totals = history_totals;
    xSemaphoreGive(state_lock);
}

esp_err_t moonraker_request_history_totals(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (history_totals.loading) {
        xSemaphoreGive(state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    history_totals.loading = true;
    history_totals.error[0] = '\0';
    ++history_totals.generation;
    xSemaphoreGive(state_lock);

    history_totals_request_id = send_rpc("server.history.totals", "{}");
    if (history_totals_request_id >= 0) return ESP_OK;

    xSemaphoreTake(state_lock, portMAX_DELAY);
    history_totals.loading = false;
    strlcpy(history_totals.error, "Moonraker unavailable", sizeof(history_totals.error));
    ++history_totals.generation;
    xSemaphoreGive(state_lock);
    return ESP_ERR_INVALID_STATE;
}

bool moonraker_get_history_job(size_t index, moonraker_history_job_t *job)
{
    if (job == NULL) return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (index >= history.count) {
        xSemaphoreGive(state_lock);
        return false;
    }
    if (history_jobs == NULL) {
        xSemaphoreGive(state_lock);
        return false;
    }
    *job = history_jobs[index];
    xSemaphoreGive(state_lock);
    return true;
}

void moonraker_release_history(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    free(history_jobs);
    history_jobs = NULL;
    history.count = 0;
    history.valid = false;
    history.loading = false;
    history_request_id = -1;
    history_totals_request_id = -1;
    ++history.generation;
    xSemaphoreGive(state_lock);
}

esp_err_t moonraker_request_history(size_t offset)
{
    char params[80];
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (history.loading || history.unavailable) {
        xSemaphoreGive(state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (history_jobs == NULL) {
        history_jobs = calloc(MOONRAKER_HISTORY_PAGE_SIZE, sizeof(*history_jobs));
        if (history_jobs == NULL) {
            strlcpy(history.error, "Not enough memory for history", sizeof(history.error));
            ++history.generation;
            xSemaphoreGive(state_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    history.offset = offset;
    history.loading = true;
    history.valid = false;
    history.error[0] = '\0';
    ++history.generation;
    xSemaphoreGive(state_lock);
    /* Request a stable newest-first order.  This prevents a server default
     * (or future configuration change) from putting old jobs on page one. */
    snprintf(params, sizeof(params), "{\"limit\":%u,\"start\":%u,\"order\":\"desc\"}",
             (unsigned)MOONRAKER_HISTORY_PAGE_SIZE, (unsigned)offset);
    history_request_id = send_rpc("server.history.list", params);
    if (history_request_id >= 0) {
        /* List replies contain the selected page's count, not the number of
         * stored jobs.  Request totals alongside it to drive real pagination. */
        (void)moonraker_request_history_totals();
        return ESP_OK;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    history.loading = false;
    strlcpy(history.error, "Moonraker unavailable", sizeof(history.error));
    ++history.generation;
    xSemaphoreGive(state_lock);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_request_macros(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (macros.loading) {
        xSemaphoreGive(state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Preserve the last successful list while a refresh is in flight.  Macro
     * configuration is commonly edited outside the pendant, so callers can
     * keep rendering the cached list instead of replacing it with a loader. */
    macros.loading = true;
    macros.error[0] = '\0';
    ++macros.generation;
    xSemaphoreGive(state_lock);
    macros_request_id = send_rpc("printer.objects.list", "{}");
    if (macros_request_id >= 0) return ESP_OK;

    xSemaphoreTake(state_lock, portMAX_DELAY);
    macros.loading = false;
    strlcpy(macros.error, "Moonraker unavailable", sizeof(macros.error));
    ++macros.generation;
    xSemaphoreGive(state_lock);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_request_directory(const char *path)
{
    char quoted_path[196];
    char params[240];
    if (!json_quote(path, quoted_path, sizeof(quoted_path))) return ESP_ERR_INVALID_ARG;
    /* A directory with metadata for every G-code can exceed the websocket
     * frame limit.  Load the compact listing here; metadata belongs to the
     * selected file only. */
    snprintf(params, sizeof(params), "{\"path\":%s,\"extended\":false}", quoted_path);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    strlcpy(file_browser.path, path, sizeof(file_browser.path));
    file_browser.loading = true;
    file_browser.valid = false;
    file_browser.error[0] = '\0';
    ++file_browser.generation;
    xSemaphoreGive(state_lock);
    directory_request_id = send_rpc("server.files.get_directory", params);
    if (directory_request_id < 0) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        file_browser.loading = false;
        strlcpy(file_browser.error, "Moonraker unavailable", sizeof(file_browser.error));
        ++file_browser.generation;
        xSemaphoreGive(state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t moonraker_request_file_metadata(const char *path)
{
    char quoted_path[196];
    char params[220];
    if (!json_quote(path, quoted_path, sizeof(quoted_path))) return ESP_ERR_INVALID_ARG;
    snprintf(params, sizeof(params), "{\"filename\":%s}", quoted_path);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    memset(&file_metadata.file, 0, sizeof(file_metadata.file));
    strlcpy(file_metadata.path, path, sizeof(file_metadata.path));
    const char *name = strrchr(path, '/');
    strlcpy(file_metadata.file.name, name == NULL ? path : name + 1, sizeof(file_metadata.file.name));
    file_metadata.loading = true;
    file_metadata.valid = false;
    file_metadata.error[0] = '\0';
    ++file_metadata.generation;
    xSemaphoreGive(state_lock);
    metadata_request_id = send_rpc("server.files.metadata", params);
    if (metadata_request_id < 0) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        file_metadata.loading = false;
        strlcpy(file_metadata.error, "Moonraker unavailable", sizeof(file_metadata.error));
        ++file_metadata.generation;
        xSemaphoreGive(state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t send_print_command(const char *method, const char *filename)
{
    char quoted_filename[196];
    char params[220];
    const char *request_params = "{}";
    if (filename != NULL) {
        if (!json_quote(filename, quoted_filename, sizeof(quoted_filename))) return ESP_ERR_INVALID_ARG;
        snprintf(params, sizeof(params), "{\"filename\":%s}", quoted_filename);
        request_params = params;
    }
    return send_rpc(method, request_params) >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_print_start(const char *filename) { return send_print_command("printer.print.start", filename); }
esp_err_t moonraker_print_pause(void) { return send_print_command("printer.print.pause", NULL); }
esp_err_t moonraker_print_resume(void) { return send_print_command("printer.print.resume", NULL); }
esp_err_t moonraker_print_cancel(void) { return send_print_command("printer.print.cancel", NULL); }

esp_err_t moonraker_emergency_stop(void)
{
    settings_printer_t config;
    ESP_RETURN_ON_ERROR(settings_get_printer(&config), TAG, "read printer configuration");
    if (!settings_printer_is_valid(&config) || !config.enabled) return ESP_ERR_INVALID_STATE;

    char uri[128];
    if (!build_emergency_stop_uri(&config, uri, sizeof(uri))) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t http_config = {
        .url = uri,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
        .buffer_size = 256,
        .buffer_size_tx = 256,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "emergency-stop HTTP request failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "emergency-stop HTTP request returned %d", status_code);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "emergency-stop request accepted");
    return ESP_OK;
}

esp_err_t moonraker_send_gcode(const char *script)
{
    char quoted_script[320];
    char params[352];
    if (!json_quote(script, quoted_script, sizeof(quoted_script))) {
        ESP_LOGW(TAG, "could not encode G-code script for JSON-RPC");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(params, sizeof(params), "{\"script\":%s}", quoted_script);
    gcode_request_id = send_rpc("printer.gcode.script", params);
    if (gcode_request_id >= 0) console_append("> ", script);
    return gcode_request_id >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_restart_server(void)
{
    return send_rpc("server.restart", "{}") >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_reboot_system(void)
{
    return send_rpc("machine.reboot", "{}") >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_shutdown_system(void)
{
    return send_rpc("machine.shutdown", "{}") >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t moonraker_request_gcode_store(void)
{
    if (gcode_store_request_id >= 0) return ESP_ERR_INVALID_STATE;
    gcode_store_request_id = send_rpc("server.gcode_store", "{\"count\":32}");
    if (gcode_store_request_id < 0) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    console.loading = true;
    console.error[0] = '\0';
    ++console.generation;
    xSemaphoreGive(state_lock);
    return ESP_OK;
}

void moonraker_get_console(moonraker_console_t *snapshot)
{
    if (snapshot == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *snapshot = console;
    xSemaphoreGive(state_lock);
}

void moonraker_get_prompt(moonraker_prompt_t *snapshot)
{
    if (snapshot == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *snapshot = prompt;
    xSemaphoreGive(state_lock);
}

void moonraker_dismiss_prompt(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (prompt.visible) {
        prompt.visible = false;
        ++prompt.generation;
    }
    xSemaphoreGive(state_lock);
}

void moonraker_get_manual_probe(moonraker_manual_probe_t *snapshot)
{
    if (snapshot == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *snapshot = manual_probe;
    xSemaphoreGive(state_lock);
}

esp_err_t moonraker_request_exclude_objects(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (exclude_objects == NULL) exclude_objects = calloc(1, sizeof(*exclude_objects));
    if (exclude_objects == NULL) { xSemaphoreGive(state_lock); return ESP_ERR_NO_MEM; }
    if (exclude_objects->loading) { xSemaphoreGive(state_lock); return ESP_ERR_INVALID_STATE; }
    exclude_objects->loading = true;
    exclude_objects->error[0] = '\0';
    ++exclude_objects->generation;
    xSemaphoreGive(state_lock);
    exclude_objects_request_id = send_rpc("printer.objects.query",
        "{\"objects\":{\"exclude_object\":null}}");
    if (exclude_objects_request_id >= 0) return ESP_OK;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    exclude_objects->loading = false;
    strlcpy(exclude_objects->error, "Moonraker unavailable", sizeof(exclude_objects->error));
    ++exclude_objects->generation;
    xSemaphoreGive(state_lock);
    return ESP_ERR_INVALID_STATE;
}

void moonraker_get_exclude_objects(moonraker_exclude_objects_t *snapshot)
{
    if (snapshot == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (exclude_objects == NULL) memset(snapshot, 0, sizeof(*snapshot));
    else *snapshot = *exclude_objects;
    xSemaphoreGive(state_lock);
}

void moonraker_mark_object_excluded(const char *name)
{
    if (name == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (exclude_objects != NULL) {
        for (size_t i = 0; i < exclude_objects->count; ++i) {
            if (strcmp(exclude_objects->objects[i].name, name) == 0) {
                exclude_objects->objects[i].excluded = true;
                ++exclude_objects->generation;
                break;
            }
        }
    }
    xSemaphoreGive(state_lock);
}

void moonraker_release_exclude_objects(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    ignored_exclude_objects_request_id = exclude_objects_request_id;
    exclude_objects_request_id = -1;
    if (exclude_objects != NULL) free(exclude_objects->objects);
    free(exclude_objects);
    exclude_objects = NULL;
    xSemaphoreGive(state_lock);
}

void moonraker_console_record_local(const char *text)
{
    console_append("> ", text);
}

esp_err_t moonraker_get_printer_state(printer_state_t *state, void *context)
{
    (void)context;
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    settings_printer_t config;
    ESP_RETURN_ON_ERROR(settings_get_printer(&config), TAG, "read printer configuration");
    if (!settings_printer_is_valid(&config) || !config.enabled) {
        memset(state, 0, sizeof(*state));
        strlcpy(state->name, "Printer", sizeof(state->name));
        state->connection = PRINTER_CONNECTION_UNCONFIGURED;
        return ESP_OK;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    *state = cached_printer;
    strlcpy(state->name, config.name, sizeof(state->name));
    xSemaphoreGive(state_lock);
    return ESP_OK;
}
