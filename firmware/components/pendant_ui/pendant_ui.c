#include "pendant_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "board.h"
#include "moonraker.h"
#include "ota_update.h"
#include "printer.h"
#include "settings.h"
#include "ui_kit.h"
#include "wifi_manager.h"

typedef enum {
    PAGE_FLEET,
    PAGE_STATUS,
    PAGE_TUNE,
    PAGE_EXCLUDE_OBJECTS,
    PAGE_EXCLUDE_CONFIRM,
    PAGE_MOVE,
    PAGE_MACROS,
    PAGE_DATA,
    PAGE_FILES,
    PAGE_FILE_DETAILS,
    PAGE_HISTORY,
    PAGE_HISTORY_DETAILS,
    PAGE_SERVICE,
    PAGE_PROBE_CALIBRATE,
    PAGE_PROMPT,
    PAGE_SYSTEM_INFO,
    PAGE_CONSOLE,
    PAGE_START_PRINT,
    PAGE_SETTINGS,
    PAGE_PRINTER_LIST,
    PAGE_NETWORK_SETUP,
    PAGE_PRINTER_SETUP,
    PAGE_DIAGNOSTICS,
    PAGE_BATTERY,
    PAGE_FIRMWARE,
    PAGE_BUTTON_MAPPING,
} page_t;

typedef enum {
    EDIT_WIFI_SSID,
    EDIT_WIFI_PASSWORD,
    EDIT_PRINTER_NAME,
    EDIT_PRINTER_HOST,
    EDIT_PRINTER_PORT,
    EDIT_CONSOLE_COMMAND,
    EDIT_LEFT_BUTTON_GCODE,
    EDIT_RIGHT_BUTTON_GCODE,
} edit_field_t;

typedef enum {
    MOVE_MODE_BROWSE,
    MOVE_MODE_AXIS,
    MOVE_MODE_EXTRUDER,
    MOVE_MODE_EXTRUDER_SPEED,
    MOVE_MODE_STEP,
    MOVE_MODE_PARK,
} move_mode_t;

typedef enum {
    TUNE_MODE_BROWSE,
    TUNE_MODE_OFFSET,
    TUNE_MODE_SPEED,
    TUNE_MODE_FLOW,
    TUNE_MODE_FAN,
    TUNE_MODE_HEATER,
} tune_mode_t;

static lv_group_t *navigation_group;
static lv_obj_t *page_title;
static lv_obj_t *page_body;
static lv_obj_t *back_hint;
static lv_obj_t *battery_label;
static lv_obj_t *wifi_signal_icon;
static lv_obj_t *wifi_signal_bars[3];
static lv_obj_t *header_printer_button;
static lv_obj_t *header_printer_label;
static lv_obj_t *header_connection_dot;
static lv_obj_t *command_feedback_label;
static lv_timer_t *command_feedback_timer;
/* Input arrives from pendant_input's task.  Never make that task wait for
 * the LVGL mutex: a busy page then lets the quadrature-edge queue overflow
 * before the next encoder state can be decoded. */
#define UI_INPUT_QUEUE_LENGTH 64
#define UI_INPUT_EVENTS_PER_TICK 8
static QueueHandle_t input_event_queue;
static bool command_feedback_visible;
static lv_obj_t *bottom_navigation[5];
static lv_obj_t *diagnostics_label;
static lv_obj_t *battery_debug_label;
static lv_obj_t *editor_overlay;
static lv_obj_t *editor_textarea;
static lv_obj_t *editor_error_label;
static lv_obj_t *console_output;
static lv_obj_t *console_output_label;
static lv_obj_t *console_new_rows;
/* These are deliberately allocated in PSRAM only while Console is open.
 * Keeping two complete views of the journal in internal BSS starves LVGL's
 * normal page allocations on this board. */
static moonraker_console_t *console_snapshot;
static char *console_rendered_text;
static uint32_t console_generation_seen;
static bool console_live = true;
static uint32_t console_pending_count;
static char console_command[241];
static lv_obj_t *fleet_printer_button;
static lv_obj_t *status_connection_button;
static lv_obj_t *status_state_button;
static lv_obj_t *status_job_button;
static lv_obj_t *status_hotend_button;
static lv_obj_t *status_bed_button;
static lv_obj_t *monitor_state_label;
static bool monitor_status_pulsing;
static lv_obj_t *monitor_job_label;
static lv_obj_t *monitor_progress_label;
static lv_obj_t *monitor_progress_bar;
static lv_obj_t *monitor_total_time_label;
static lv_obj_t *monitor_eta_label;
static char monitor_metadata_filename[96];
static uint32_t monitor_estimated_time_seconds;
static char monitor_completed_filename[96];
static uint32_t monitor_completed_duration_seconds;
static lv_obj_t *monitor_hotend_label;
static lv_obj_t *monitor_bed_label;
static lv_obj_t *monitor_position_label;
static lv_obj_t *monitor_pause_action;
static lv_obj_t *monitor_pause_action_label;
static moonraker_file_entry_t selected_file;
static char selected_file_path[96];
static uint32_t files_generation_seen;
static size_t files_page_offset;
static lv_obj_t *files_pagination;
static lv_obj_t *files_previous_page_button;
static lv_obj_t *files_next_page_button;
static lv_obj_t *files_page_label;
static uint32_t metadata_generation_seen;
static uint32_t macros_generation_seen;
static moonraker_macro_list_t macros_snapshot;
static moonraker_disk_info_t disk_info_snapshot;
static uint32_t disk_info_generation_seen;
static moonraker_manual_probe_t manual_probe_snapshot;
static uint32_t manual_probe_generation_seen;
static moonraker_prompt_t prompt_snapshot;
static uint32_t prompt_generation_seen;
static page_t prompt_return_page = PAGE_STATUS;
/* Kept out of the LVGL task stack: directory pages contain several rich rows. */
static moonraker_file_browser_t file_browser_snapshot;
static moonraker_file_browser_page_t file_browser_page_snapshot;
/* The first entry returned by Moonraker History is the most recently started
 * job.  Keep a copy while the Start print page is open because the bounded
 * History cache is released as soon as we leave that page. */
static moonraker_history_job_t last_print_job;
static moonraker_history_status_t history_snapshot;
static moonraker_history_totals_t history_totals_snapshot;
static moonraker_history_job_t selected_history_job;
static uint32_t history_generation_seen;
static uint32_t history_totals_generation_seen;
static page_t current_page;
static edit_field_t editor_field;
static settings_wifi_t editable_wifi;
static settings_printer_t editable_printer;
static settings_button_mapping_t editable_button_mapping;
static bool button_mapping_editing;
static bool button_mapping_edit_left;
static settings_button_action_t button_mapping_original_action;
static lv_obj_t *button_mapping_row;
static settings_printer_t printer_list_snapshot[SETTINGS_MAX_PRINTERS];
static size_t editing_printer_index = SIZE_MAX;
/* Capability snapshots are intentionally kept off the small app_main/LVGL
 * caller stack. The bounded arrays make this object several KiB large. */
static printer_capabilities_t tune_capabilities;
static tune_mode_t tune_mode;
static uint8_t tune_item_index;
static int32_t tune_value;
static int32_t tune_original_value;
static int64_t tune_last_rotate_us;
static lv_obj_t *tune_value_row;
static lv_obj_t *tune_offset_row;
static lv_obj_t *tune_speed_row;
static lv_obj_t *tune_flow_row;
static lv_obj_t *tune_fan_rows[PRINTER_MAX_FANS];
static lv_obj_t *tune_heater_rows[PRINTER_MAX_HEATERS];
static int32_t tune_offset_centi_mm;
static int32_t tune_speed_percent = 100;
static int32_t tune_flow_percent = 100;
/* Allocated from PSRAM only while the object-map page is open. */
static moonraker_exclude_objects_t *exclude_objects_snapshot;
static uint32_t exclude_objects_generation_seen;
static uint8_t exclude_selected_index;
static lv_point_t *exclude_map_points;
static bool printer_selection_pending;
static move_mode_t move_mode;
static uint8_t move_axis;
static uint8_t move_step_index = 2;
static uint8_t move_park_index;
static int32_t move_distance_centi_mm;
static int64_t move_last_rotate_us;
static lv_obj_t *move_value_row;
static lv_obj_t *move_axis_rows[3];
static lv_obj_t *move_position_section;
static lv_obj_t *move_jog_section;
static lv_obj_t *move_extrusion_section;
static lv_obj_t *move_extruder_feed_row;
static lv_obj_t *move_extruder_speed_row;
static lv_obj_t *move_step_row;
static lv_obj_t *move_park_row;
/* The UI uses the customary extrusion unit.  G-code conversion to mm/min is
 * performed only when the command is sent. */
static int32_t move_extruder_speed_mm_s = 5;
static ota_update_state_t firmware_state_seen = OTA_UPDATE_IDLE;
static uint8_t firmware_progress_seen;
static printer_connection_t navigation_connection_seen = PRINTER_CONNECTION_UNCONFIGURED;

static void navigate_to(page_t page);
static bool page_requires_online_printer(page_t page);
static void open_editor(edit_field_t field);
static void handle_input_locked(const pendant_input_event_t *event);
static bool printer_is_error(const printer_state_t *printer);
static bool printer_is_printing(const printer_state_t *printer);
static bool print_start_available(const printer_state_t *printer);
static void move_axis_event(lv_event_t *event);
static void move_extruder_feed_event(lv_event_t *event);
static void move_extruder_speed_event(lv_event_t *event);
static void move_step_event(lv_event_t *event);
static void move_park_event(lv_event_t *event);
static void move_confirm_event(lv_event_t *event);
static void move_cancel_event(lv_event_t *event);
static void emergency_stop_event(lv_event_t *event);
static void button_mapping_event(lv_event_t *event);
static void refresh_move_rows(const printer_state_t *printer);
static void refresh_tune_rows(const printer_state_t *printer);
static void refresh_pause_action(const printer_state_t *printer);
static void start_monitor_status_pulse(const printer_state_t *printer);
static void move_exit_editing(void);
static void tune_value_event(lv_event_t *event);
static void tune_confirm_event(lv_event_t *event);
static void tune_cancel_event(lv_event_t *event);
static void tune_exit_editing(void);
static void exclude_objects_event(lv_event_t *event);
static void exclude_object_select_event(lv_event_t *event);
static void exclude_object_confirm_event(lv_event_t *event);
static void tune_update_value_row(void);
static void pause_print_event(lv_event_t *event);
static void resume_print_event(lv_event_t *event);
static void toggle_pause_print_event(lv_event_t *event);
static void cancel_print_event(lv_event_t *event);
static void service_gcode_event(lv_event_t *event);
static void probe_gcode_event(lv_event_t *event);
static void prompt_button_event(lv_event_t *event);
static void macro_event(lv_event_t *event);
static void files_previous_page_event(lv_event_t *event);
static void files_next_page_event(lv_event_t *event);
static void update_files_pagination(void);
static void update_history_pagination(void);
static void report_command_result(const char *action, esp_err_t result);
static void firmware_check_event(lv_event_t *event);
static void firmware_install_release_event(lv_event_t *event);
static void open_last_print_event(lv_event_t *event);
static void start_print_files_event(lv_event_t *event);
static void history_entry_event(lv_event_t *event);
static void format_print_time(char *text, size_t text_size, uint32_t seconds);
static void format_total_print_time(char *text, size_t text_size, uint64_t seconds);
static void format_timestamp(char *text, size_t text_size, uint64_t milliseconds);
static void format_history_list_time(char *text, size_t text_size, uint64_t milliseconds);
static void format_storage_size(char *text, size_t text_size, uint64_t bytes);
static void console_command_event(lv_event_t *event);
static void console_send_event(lv_event_t *event);
static void console_output_event(lv_event_t *event);
static void console_new_rows_event(lv_event_t *event);
static void refresh_console_output(void);
static bool console_buffers_acquire(void);
static void console_buffers_release(void);

#define CONSOLE_KB_BTN(width) (LV_BTNMATRIX_CTRL_POPOVER | (width))

/* The stock keyboard reserves its bottom-left key for hiding itself.  That
 * action is not useful in a modal editor, so Console uses the space for the
 * missing cursor-left key.  The special layout keeps '.' and ',' for IPs. */
static const char *console_kb_lower[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char *console_kb_upper[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char *console_kb_special[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", ".", ",", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t console_kb_ctrl[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4),
    CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), CONSOLE_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3),
    CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3), CONSOLE_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_BTNMATRIX_CTRL_CHECKED | CONSOLE_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | CONSOLE_KB_BTN(1), CONSOLE_KB_BTN(1),
    CONSOLE_KB_BTN(1), CONSOLE_KB_BTN(1), CONSOLE_KB_BTN(1), CONSOLE_KB_BTN(1), CONSOLE_KB_BTN(1), CONSOLE_KB_BTN(1),
    LV_BTNMATRIX_CTRL_CHECKED | CONSOLE_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | CONSOLE_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | CONSOLE_KB_BTN(1),
    LV_BTNMATRIX_CTRL_CHECKED | 2, 7, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 3
};

static void touch_focus_event(lv_event_t *event)
{
    if (navigation_group != NULL) lv_group_focus_obj(lv_event_get_target(event));
}

/*
 * Physical input is decoded in pendant_input's FreeRTOS task.  Do not run
 * focus changes from that task: focusing a row can synchronously start a
 * scroll animation and needs considerably more stack than input decoding.
 * The input queue is consumed by an LVGL timer in its own task.
 */
static void input_event_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* This callback executes in LVGL's own task, so it may safely update
     * focus and start scrolling.  Bound the work per pass to retain touch and
     * display responsiveness during a fast turn. */
    pendant_input_event_t event;
    for (uint8_t i = 0; i < UI_INPUT_EVENTS_PER_TICK &&
                        xQueueReceive(input_event_queue, &event, 0) == pdTRUE; ++i) {
        handle_input_locked(&event);
    }
}

/* Inline editing is intentionally stronger than ordinary focus: a green 4 px
 * frame says that the encoder changes this value now. The value itself keeps
 * its semantic color. */
static void set_inline_row_visual(lv_obj_t *row, bool editing)
{
    if (row == NULL) return;
    const lv_color_t frame_color = editing ? ui_kit_tone_color(UI_TONE_SUCCESS) :
                                             ui_kit_color(UI_COLOR_BORDER_FOCUSED);
    lv_obj_set_style_border_color(row, frame_color,
                                  LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(row, frame_color,
                                   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(row, editing ? ui_kit_color(UI_COLOR_SURFACE) :
                                             ui_kit_color(UI_COLOR_SURFACE_FOCUSED),
                              LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(row, editing ? 4 : 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(row, editing ? 3 : 2, LV_PART_MAIN | LV_STATE_FOCUSED);
}

static void command_feedback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    command_feedback_visible = false;
    if (command_feedback_label != NULL) lv_obj_add_flag(command_feedback_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

/* Moonraker accepts commands asynchronously.  Say "requested" rather than
 * claiming completion; the normal printer-state refresh remains authoritative. */
static void report_command_result(const char *action, esp_err_t result)
{
    if (command_feedback_label == NULL) return;
    if (result == ESP_OK) {
        lv_label_set_text_fmt(command_feedback_label, LV_SYMBOL_OK " %s requested", action);
        lv_obj_set_style_text_color(command_feedback_label, ui_kit_tone_color(UI_TONE_SUCCESS), LV_PART_MAIN);
    } else {
        lv_label_set_text_fmt(command_feedback_label, LV_SYMBOL_WARNING " %s failed", action);
        lv_obj_set_style_text_color(command_feedback_label, ui_kit_tone_color(UI_TONE_DANGER), LV_PART_MAIN);
    }
    command_feedback_visible = true;
    lv_obj_clear_flag(command_feedback_label, LV_OBJ_FLAG_HIDDEN);
    if (command_feedback_timer != NULL) {
        lv_timer_reset(command_feedback_timer);
        lv_timer_resume(command_feedback_timer);
    }
}

static page_t parent_page(page_t page)
{
    switch (page) {
    case PAGE_NETWORK_SETUP:
    case PAGE_PRINTER_LIST:
    case PAGE_DIAGNOSTICS:
    case PAGE_BATTERY:
    case PAGE_FIRMWARE:
    case PAGE_BUTTON_MAPPING:
        return PAGE_SETTINGS;
    case PAGE_PRINTER_SETUP:
        return PAGE_PRINTER_LIST;
    case PAGE_FILE_DETAILS:
        return PAGE_FILES;
    case PAGE_HISTORY_DETAILS:
        return PAGE_HISTORY;
    case PAGE_HISTORY:
        return PAGE_DATA;
    case PAGE_FILES:
        return PAGE_DATA;
    case PAGE_MACROS:
        return PAGE_MOVE;
    case PAGE_START_PRINT:
        return PAGE_STATUS;
    case PAGE_EXCLUDE_OBJECTS:
        return PAGE_TUNE;
    case PAGE_EXCLUDE_CONFIRM:
        return PAGE_EXCLUDE_OBJECTS;
    case PAGE_CONSOLE:
    case PAGE_SYSTEM_INFO:
    case PAGE_PROBE_CALIBRATE:
        return PAGE_SERVICE;
    case PAGE_PROMPT:
        return prompt_return_page;
    case PAGE_STATUS:
    case PAGE_TUNE:
    case PAGE_MOVE:
    case PAGE_DATA:
    case PAGE_SERVICE:
    case PAGE_SETTINGS:
    case PAGE_FLEET:
    default:
        return PAGE_FLEET;
    }
}

static const char *printer_connection_text(printer_connection_t connection)
{
    switch (connection) {
    case PRINTER_CONNECTION_ONLINE: return "online";
    case PRINTER_CONNECTION_OFFLINE: return "offline";
    case PRINTER_CONNECTION_UNCONFIGURED: return "not configured";
    default: return "unknown";
    }
}

static const char *printer_state_text(printer_operational_state_t state)
{
    switch (state) {
    case PRINTER_STATE_STARTUP: return "startup";
    case PRINTER_STATE_READY: return "ready";
    case PRINTER_STATE_ERROR: return "error";
    case PRINTER_STATE_SHUTDOWN: return "shutdown";
    case PRINTER_STATE_UNKNOWN: return "unknown";
    default: return "unknown";
    }
}

static const char *printer_job_text(printer_job_state_t state)
{
    switch (state) {
    case PRINTER_JOB_STANDBY: return "standby";
    case PRINTER_JOB_PRINTING: return "printing";
    case PRINTER_JOB_PAUSED: return "paused";
    case PRINTER_JOB_COMPLETE: return "complete";
    case PRINTER_JOB_CANCELLED: return "cancelled";
    case PRINTER_JOB_ERROR: return "error";
    case PRINTER_JOB_UNKNOWN: return "unknown";
    default: return "unknown";
    }
}

static void set_nav_button_text(lv_obj_t *button, const char *text)
{
    ui_kit_set_row_text(button, text, NULL);
}

/* Match the axis-state palette used throughout the motion UI. */
static const char *axis_homing_color(bool homed)
{
    if (ui_kit_get_theme() == UI_THEME_DARK) return homed ? "3FCB6E" : "5C5C5C";
    return homed ? "1E9E52" : "B0B0B2";
}

static void format_position(char *text, size_t text_size, const printer_state_t *printer)
{
    static const char *const axis_labels[] = { "X", "Y", "Z" };
    const int32_t positions[] = { printer->toolhead_x_centi_mm,
                                  printer->toolhead_y_centi_mm,
                                  printer->toolhead_z_centi_mm };
    if (!printer->toolhead_position_valid) {
        snprintf(text, text_size, "#%s %s# --.-    #%s %s# --.-    #%s %s# --.- mm",
                 axis_homing_color(printer->toolhead_axes_homed[0]), axis_labels[0],
                 axis_homing_color(printer->toolhead_axes_homed[1]), axis_labels[1],
                 axis_homing_color(printer->toolhead_axes_homed[2]), axis_labels[2]);
        return;
    }
    snprintf(text, text_size, "#%s %s# %ld.%ld    #%s %s# %ld.%ld    #%s %s# %ld.%ld mm",
             axis_homing_color(printer->toolhead_axes_homed[0]), axis_labels[0],
             (long)(positions[0] / 100), labs((long)positions[0] % 100) / 10,
             axis_homing_color(printer->toolhead_axes_homed[1]), axis_labels[1],
             (long)(positions[1] / 100), labs((long)positions[1] % 100) / 10,
             axis_homing_color(printer->toolhead_axes_homed[2]), axis_labels[2],
             (long)(positions[2] / 100), labs((long)positions[2] % 100) / 10);
}

static void format_duration(char *text, size_t text_size, uint32_t seconds)
{
    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds % 3600) / 60;
    if (hours > 0) snprintf(text, text_size, "%luh %02lum", (unsigned long)hours,
                            (unsigned long)minutes);
    else snprintf(text, text_size, "%lum", (unsigned long)minutes);
}

static void format_monitor_print_time(char *total, size_t total_size, char *eta, size_t eta_size,
                                      const printer_state_t *printer, uint32_t estimated_time_seconds)
{
    if (printer->job_state == PRINTER_JOB_COMPLETE && printer->progress_tenths_percent >= 1000) {
        uint32_t duration = printer->print_duration_seconds;
        if (duration == 0 && strcmp(printer->filename, monitor_completed_filename) == 0) {
            duration = monitor_completed_duration_seconds;
        }
        if (duration > 0) {
            char text[20];
            format_duration(text, sizeof(text), duration);
            snprintf(total, total_size, "TOTAL %s", text);
            strlcpy(eta, "DONE", eta_size);
            return;
        }
    }
    if (!printer_is_printing(printer)) {
        strlcpy(total, "TOTAL --", total_size);
        strlcpy(eta, "ETA -- left", eta_size);
        return;
    }

    uint32_t estimated = estimated_time_seconds;
    uint32_t remaining = 0;
    if (estimated > 0) {
        /* Moonraker's primary ETA method: metadata estimate minus the
         * completed proportion reported by virtual_sdcard. */
        uint64_t completed = ((uint64_t)estimated * printer->progress_tenths_percent) / 1000U;
        remaining = completed < estimated ? (uint32_t)(estimated - completed) : 0;
    } else if (printer->progress_tenths_percent > 0 && printer->print_duration_seconds > 0) {
        /* Metadata is optional; use Moonraker's duration/progress fallback. */
        estimated = (uint32_t)(((uint64_t)printer->print_duration_seconds * 1000U) /
                               printer->progress_tenths_percent);
        remaining = estimated > printer->print_duration_seconds ?
                    estimated - printer->print_duration_seconds : 0;
    } else {
        strlcpy(total, "TOTAL --", total_size);
        strlcpy(eta, "ETA -- left", eta_size);
        return;
    }
    char duration[20];
    format_duration(duration, sizeof(duration), (uint32_t)estimated);
    snprintf(total, total_size, "TOTAL ~%s", duration);
    format_duration(duration, sizeof(duration), remaining);
    snprintf(eta, eta_size, "ETA %s left", duration);
}

static void update_monitor_metadata(const printer_state_t *printer)
{
    if (printer->filename[0] == '\0' ||
        (!printer_is_printing(printer) && printer->job_state != PRINTER_JOB_COMPLETE)) {
        monitor_metadata_filename[0] = '\0';
        monitor_estimated_time_seconds = 0;
        return;
    }

    if (printer->print_duration_seconds > 0) {
        strlcpy(monitor_completed_filename, printer->filename, sizeof(monitor_completed_filename));
        monitor_completed_duration_seconds = printer->print_duration_seconds;
    }

    /* Keep the completed job's metadata and duration visible.  It is cleared
     * on the next idle job transition or replaced when a new print starts. */
    if (!printer_is_printing(printer)) return;

    if (strcmp(monitor_metadata_filename, printer->filename) != 0) {
        monitor_estimated_time_seconds = 0;
        if (moonraker_request_file_metadata(printer->filename) == ESP_OK) {
            strlcpy(monitor_metadata_filename, printer->filename,
                    sizeof(monitor_metadata_filename));
        }
        return;
    }

    moonraker_file_metadata_t metadata;
    moonraker_get_file_metadata(&metadata);
    if (metadata.valid && strcmp(metadata.path, printer->filename) == 0) {
        monitor_estimated_time_seconds = metadata.file.estimated_time_seconds;
    }
}

static void refresh_printer_page(void)
{
    printer_state_t printer;
    if (printer_get_state(&printer) != ESP_OK) return;
    /* The Monitor layout contains a different final row while a job is
     * active: Pause/Resume + Cancel instead of Start print (or no action).
     * Its widgets are created when the page is built, so changing only their
     * labels leaves stale controls after a job ends and omits them when a job
     * starts while Monitor is already open. Rebuild once per active-job
     * transition; ordinary telemetry updates still take the cheap path.
     *
     * monitor_state_label also prevents this from running during initial UI
     * construction, before PAGE_STATUS has populated its widget pointers.
     */
    if (current_page == PAGE_STATUS && monitor_state_label != NULL &&
        (monitor_pause_action != NULL) != printer_is_printing(&printer)) {
        navigate_to(PAGE_STATUS);
        return;
    }
    char text[128];
    if (monitor_state_label != NULL) update_monitor_metadata(&printer);
    if (header_printer_label != NULL) {
        lv_label_set_text(header_printer_label, printer.name);
    }
    if (header_connection_dot != NULL) {
        lv_obj_set_style_bg_color(header_connection_dot,
            printer.connection == PRINTER_CONNECTION_ONLINE ? ui_kit_tone_color(UI_TONE_SUCCESS) :
            printer.connection == PRINTER_CONNECTION_OFFLINE ? ui_kit_tone_color(UI_TONE_MUTED) :
            ui_kit_tone_color(UI_TONE_WARNING), LV_PART_MAIN);
    }
    refresh_move_rows(&printer);
    refresh_tune_rows(&printer);
    if (fleet_printer_button != NULL) {
        const char *status = printer.connection == PRINTER_CONNECTION_ONLINE ? "ONLINE" :
                             printer.connection == PRINTER_CONNECTION_OFFLINE ? "OFFLINE" : "SETUP";
        ui_kit_set_row_text(fleet_printer_button, printer.name, status);
        ui_kit_set_row_tone(fleet_printer_button,
                            printer.connection == PRINTER_CONNECTION_ONLINE ? UI_TONE_SUCCESS :
                            printer.connection == PRINTER_CONNECTION_OFFLINE ? UI_TONE_MUTED : UI_TONE_WARNING);
        snprintf(text, sizeof(text), "Moonraker %s", printer_connection_text(printer.connection));
        ui_kit_set_row_subtitle(fleet_printer_button, text);
    }
    if (status_connection_button != NULL) {
        snprintf(text, sizeof(text), "Moonraker: %s", printer_connection_text(printer.connection));
        set_nav_button_text(status_connection_button, text);
    }
    if (status_state_button != NULL) {
        if (printer.operational_state == PRINTER_STATE_ERROR ||
            printer.operational_state == PRINTER_STATE_SHUTDOWN) {
            snprintf(text, sizeof(text), "Printer: %s - %.56s",
                     printer_state_text(printer.operational_state), printer.state_message);
        } else {
            snprintf(text, sizeof(text), "Printer: %s",
                     printer_state_text(printer.operational_state));
        }
        set_nav_button_text(status_state_button, text);
    }
    if (status_job_button != NULL) {
        if (printer.job_state == PRINTER_JOB_ERROR && printer.job_message[0] != '\0') {
            snprintf(text, sizeof(text), "Job: error - %.60s", printer.job_message);
        } else {
            snprintf(text, sizeof(text), "Job: %s", printer_job_text(printer.job_state));
        }
        set_nav_button_text(status_job_button, text);
    }
    if (status_hotend_button != NULL) {
        snprintf(text, sizeof(text), "Hotend: %d.%d / %d.%d C",
                 printer.hotend_current_deci_c / 10, abs(printer.hotend_current_deci_c % 10),
                 printer.hotend_target_deci_c / 10, abs(printer.hotend_target_deci_c % 10));
        set_nav_button_text(status_hotend_button, text);
    }
    if (status_bed_button != NULL) {
        snprintf(text, sizeof(text), "Bed: %d.%d / %d.%d C",
                 printer.bed_current_deci_c / 10, abs(printer.bed_current_deci_c % 10),
                 printer.bed_target_deci_c / 10, abs(printer.bed_target_deci_c % 10));
        set_nav_button_text(status_bed_button, text);
    }
    if (monitor_state_label != NULL) {
        const char *state = printer_is_error(&printer) ? "ERROR" :
                            printer.job_state == PRINTER_JOB_PAUSED ? "PAUSED" :
                            printer.job_state == PRINTER_JOB_PRINTING ? "PRINTING" :
                            printer.operational_state == PRINTER_STATE_READY ? "READY" : "OFFLINE";
        lv_label_set_text(monitor_state_label, state);
        lv_obj_set_style_text_color(monitor_state_label,
            printer_is_error(&printer) ? ui_kit_tone_color(UI_TONE_DANGER) :
            printer_is_printing(&printer) ? ui_kit_tone_color(UI_TONE_SUCCESS) :
            ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
        start_monitor_status_pulse(&printer);
    }
    if (monitor_job_label != NULL) {
        const char *filename = printer.filename[0] ? printer.filename : "No active print";
        /* Do not restart the marquee on every state refresh. */
        if (strcmp(lv_label_get_text(monitor_job_label), filename) != 0) {
            lv_label_set_text(monitor_job_label, filename);
        }
    }
    if (monitor_progress_label != NULL) {
        snprintf(text, sizeof(text), "%u.%u%%", printer.progress_tenths_percent / 10,
                 printer.progress_tenths_percent % 10);
        lv_label_set_text(monitor_progress_label, text);
    }
    if (monitor_progress_bar != NULL) {
        lv_bar_set_value(monitor_progress_bar, printer.progress_tenths_percent / 10, LV_ANIM_OFF);
    }
    if (monitor_total_time_label != NULL && monitor_eta_label != NULL) {
        char total[32];
        char eta[32];
        format_monitor_print_time(total, sizeof(total), eta, sizeof(eta), &printer,
                                  monitor_estimated_time_seconds);
        lv_label_set_text(monitor_total_time_label, total);
        lv_label_set_text(monitor_eta_label, eta);
    }
    if (monitor_hotend_label != NULL) {
        snprintf(text, sizeof(text), "%d / %d °C", printer.hotend_current_deci_c / 10,
                 printer.hotend_target_deci_c / 10);
        lv_label_set_text(monitor_hotend_label, text);
    }
    if (monitor_bed_label != NULL) {
        snprintf(text, sizeof(text), "%d / %d °C", printer.bed_current_deci_c / 10,
                 printer.bed_target_deci_c / 10);
        lv_label_set_text(monitor_bed_label, text);
    }
    if (monitor_position_label != NULL) {
        format_position(text, sizeof(text), &printer);
        lv_label_set_text(monitor_position_label, text);
    }
    refresh_pause_action(&printer);
}

static bool printer_is_error(const printer_state_t *printer)
{
    return printer->operational_state == PRINTER_STATE_ERROR ||
           printer->operational_state == PRINTER_STATE_SHUTDOWN ||
           printer->job_state == PRINTER_JOB_ERROR;
}

static bool printer_is_printing(const printer_state_t *printer)
{
    return printer->job_state == PRINTER_JOB_PRINTING ||
           printer->job_state == PRINTER_JOB_PAUSED;
}

static bool print_start_available(const printer_state_t *printer)
{
    return printer->connection == PRINTER_CONNECTION_ONLINE &&
           printer->operational_state == PRINTER_STATE_READY &&
           /* Moonraker retains the last terminal print_stats state (usually
            * "complete") after the virtual-SD job has been released.  READY
            * is authoritative here; only a live print or an error must keep
            * a new job from being started. */
           !printer_is_printing(printer) && !printer_is_error(printer);
}

static bool is_printer_page(page_t page)
{
    return page == PAGE_STATUS || page == PAGE_TUNE || page == PAGE_EXCLUDE_OBJECTS ||
           page == PAGE_EXCLUDE_CONFIRM || page == PAGE_MOVE ||
           page == PAGE_MACROS || page == PAGE_DATA || page == PAGE_FILES ||
           page == PAGE_FILE_DETAILS || page == PAGE_HISTORY || page == PAGE_HISTORY_DETAILS ||
           page == PAGE_SERVICE || page == PAGE_PROBE_CALIBRATE || page == PAGE_PROMPT ||
           page == PAGE_SYSTEM_INFO || page == PAGE_CONSOLE || page == PAGE_START_PRINT;
}

static bool page_requires_online_printer(page_t page)
{
    return page == PAGE_TUNE || page == PAGE_EXCLUDE_OBJECTS || page == PAGE_EXCLUDE_CONFIRM ||
           page == PAGE_MOVE || page == PAGE_MACROS ||
           page == PAGE_DATA || page == PAGE_FILES || page == PAGE_FILE_DETAILS ||
           page == PAGE_HISTORY || page == PAGE_HISTORY_DETAILS ||
           page == PAGE_SERVICE || page == PAGE_PROBE_CALIBRATE || page == PAGE_PROMPT ||
           page == PAGE_SYSTEM_INFO || page == PAGE_CONSOLE ||
           page == PAGE_START_PRINT;
}

static void scan_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (current_page == PAGE_NETWORK_SETUP) navigate_to(PAGE_NETWORK_SETUP);
}

static void load_editable_settings(void)
{
    settings_get_wifi(&editable_wifi);
    settings_get_printer(&editable_printer);
}

static void refresh_diagnostics(void)
{
    if (diagnostics_label == NULL) return;
    board_battery_status_t status;
    board_battery_get_status(&status);
    if (!status.valid) {
        lv_label_set_text(diagnostics_label, "LCD: ST7796U  320 x 480\nTouch: CST826 ready\nPSRAM: 8 MiB verified\nBattery: ADC unavailable\nInputs: EC11 / Back / E-STOP ready");
    } else if (!status.present) {
        lv_label_set_text(diagnostics_label, "LCD: ST7796U  320 x 480\nTouch: CST826 ready\nPSRAM: 8 MiB verified\nBattery: no battery attached\nInputs: EC11 / Back / E-STOP ready");
    } else {
        lv_label_set_text_fmt(diagnostics_label, "LCD: ST7796U  320 x 480\nTouch: CST826 ready\nPSRAM: 8 MiB verified\nBattery: %u.%02u V, %u%%%s\nInputs: EC11 / Back / E-STOP ready",
                              status.voltage_mv / 1000, (status.voltage_mv % 1000) / 10, status.percent,
                              status.charge_state == BOARD_BATTERY_CHARGING ? ", charging" : "");
    }
}

static void refresh_battery_debug(void)
{
    if (battery_debug_label == NULL) return;

    board_battery_status_t status;
    board_battery_get_status(&status);
    const char *state = status.charge_state == BOARD_BATTERY_CHARGING ? "Charging (USB)" :
                        status.charge_state == BOARD_BATTERY_DISCHARGING ? "Discharging" : "Unavailable";
    if (!status.valid) {
        lv_label_set_text(battery_debug_label, "ADC: unavailable\n\nCheck BAT connector and GPIO4 ADC.");
        return;
    }
    lv_label_set_text_fmt(battery_debug_label,
                          "Voltage     %u.%03u V\n"
                          "Estimated   %u%%\n"
                          "State       %s\n"
                          "Battery     %s\n\n"
                          "Protection\n"
                          "Warning     <= 15%%\n"
                          "Sleep       <= 3.30 V after 10 s\n"
                          "Recheck     every 30 s\n\n"
                          "Percentage is an OCV estimate. It may\n"
                          "fall after USB is unplugged as the\n"
                          "cell voltage settles.",
                          status.voltage_mv / 1000, status.voltage_mv % 1000,
                          status.percent, state, status.present ? "detected" : "not detected");
}

static void set_wifi_signal_strength(uint8_t level, ui_tone_t tone)
{
    const lv_color_t color = ui_kit_tone_color(tone);
    for (uint8_t i = 0; i < 3; ++i) {
        if (wifi_signal_bars[i] == NULL) continue;
        lv_obj_set_style_bg_color(wifi_signal_bars[i],
            i < level ? color : ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wifi_signal_bars[i], LV_OPA_COVER, LV_PART_MAIN);
    }
}

static void battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Firmware owns Wi-Fi and the screen needs only OTA state.  Do not wake
     * ADC, query Moonraker, or redraw header telemetry once per second while
     * this page is open. */
    if (current_page == PAGE_FIRMWARE) {
        ota_update_status_t update;
        ota_update_get_status(&update);
        if (update.state != firmware_state_seen || update.progress != firmware_progress_seen) {
            firmware_state_seen = update.state;
            firmware_progress_seen = update.progress;
            navigate_to(PAGE_FIRMWARE);
        }
        return;
    }
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.state == WIFI_MANAGER_CONNECTED) {
        /* RSSI is useful for diagnostics but not as a persistent header value.
         * The three bars keep the signal readable at a glance. */
        if (wifi.rssi >= -60) {
            set_wifi_signal_strength(3, UI_TONE_SUCCESS);
        } else if (wifi.rssi >= -75) {
            set_wifi_signal_strength(2, UI_TONE_WARNING);
        } else {
            set_wifi_signal_strength(1, UI_TONE_DANGER);
        }
    } else if (wifi.scanning) {
        set_wifi_signal_strength(0, UI_TONE_WARNING);
    } else {
        set_wifi_signal_strength(0, UI_TONE_MUTED);
    }
    /* Firmware updates need Wi-Fi and TLS heap, not live printer rendering. */
    refresh_printer_page();

    moonraker_get_prompt(&prompt_snapshot);
    if (prompt_snapshot.visible && current_page != PAGE_PROMPT) {
        prompt_return_page = current_page;
        prompt_generation_seen = prompt_snapshot.generation;
        navigate_to(PAGE_PROMPT);
        return;
    }

    printer_state_t printer;
    printer_get_state(&printer);
    if (printer.connection != navigation_connection_seen) {
        navigation_connection_seen = printer.connection;
        /* A disconnected printer cannot serve action, file, or service
         * screens. The first refresh runs before page_body is created during
         * UI startup, so only navigate after the screen is ready. */
        if (page_body != NULL) {
            navigate_to(page_requires_online_printer(current_page) &&
                        printer.connection != PRINTER_CONNECTION_ONLINE ? PAGE_STATUS : current_page);
        }
    }

    /* OTA work runs in a FreeRTOS task. Rebuild this page when that task
     * changes state so "Checking" is replaced by either the result or an
     * error without requiring the user to leave and reopen the page. */
    /* File updates must not depend on a battery being attached. */
    if (current_page == PAGE_FILES) {
        moonraker_get_file_browser(&file_browser_snapshot);
        if (file_browser_snapshot.generation != files_generation_seen) {
            files_generation_seen = file_browser_snapshot.generation;
            navigate_to(PAGE_FILES);
        }
    } else if (current_page == PAGE_FILE_DETAILS) {
        moonraker_file_metadata_t metadata;
        moonraker_get_file_metadata(&metadata);
        if (metadata.generation != metadata_generation_seen) {
            metadata_generation_seen = metadata.generation;
            if (metadata.valid) selected_file = metadata.file;
            navigate_to(current_page);
        }
    } else if (current_page == PAGE_MACROS) {
        moonraker_get_macros(&macros_snapshot);
        if (macros_snapshot.generation != macros_generation_seen) {
            macros_generation_seen = macros_snapshot.generation;
            navigate_to(PAGE_MACROS);
        }
    } else if (current_page == PAGE_EXCLUDE_OBJECTS && exclude_objects_snapshot != NULL) {
        moonraker_get_exclude_objects(exclude_objects_snapshot);
        if (exclude_objects_snapshot->generation != exclude_objects_generation_seen) {
            exclude_objects_generation_seen = exclude_objects_snapshot->generation;
            navigate_to(current_page);
        }
    } else if (current_page == PAGE_SYSTEM_INFO) {
        moonraker_get_disk_info(&disk_info_snapshot);
        if (disk_info_snapshot.generation != disk_info_generation_seen) {
            disk_info_generation_seen = disk_info_snapshot.generation;
            navigate_to(PAGE_SYSTEM_INFO);
        }
    } else if (current_page == PAGE_DATA) {
        moonraker_get_history_totals(&history_totals_snapshot);
        if (history_totals_snapshot.generation != history_totals_generation_seen) {
            history_totals_generation_seen = history_totals_snapshot.generation;
            navigate_to(PAGE_DATA);
        }
    } else if (current_page == PAGE_HISTORY || current_page == PAGE_HISTORY_DETAILS ||
               current_page == PAGE_START_PRINT) {
        moonraker_get_history_status(&history_snapshot);
        if (history_snapshot.generation != history_generation_seen) {
            history_generation_seen = history_snapshot.generation;
            navigate_to(current_page);
        }
    } else if (current_page == PAGE_CONSOLE) {
        if (console_snapshot != NULL) {
            moonraker_get_console(console_snapshot);
            if (console_snapshot->generation != console_generation_seen) {
                if (!console_live) ++console_pending_count;
                console_generation_seen = console_snapshot->generation;
                refresh_console_output();
            }
        }
    } else if (current_page == PAGE_PROBE_CALIBRATE) {
        moonraker_get_manual_probe(&manual_probe_snapshot);
        if (manual_probe_snapshot.generation != manual_probe_generation_seen) {
            manual_probe_generation_seen = manual_probe_snapshot.generation;
            navigate_to(PAGE_PROBE_CALIBRATE);
        }
    } else if (current_page == PAGE_PROMPT) {
        if (!prompt_snapshot.visible) {
            navigate_to(prompt_return_page);
            return;
        }
        if (prompt_snapshot.generation != prompt_generation_seen) {
            prompt_generation_seen = prompt_snapshot.generation;
            navigate_to(PAGE_PROMPT);
        }
    }

    board_battery_status_t status;
    board_battery_get_status(&status);
    if (!status.valid) {
        lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_EMPTY " --");
        lv_obj_set_style_text_color(battery_label, ui_kit_tone_color(UI_TONE_DANGER), LV_PART_MAIN);
        refresh_diagnostics();
        refresh_battery_debug();
        return;
    }
    if (!status.present) {
        lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_EMPTY " --");
        lv_obj_set_style_text_color(battery_label, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
        refresh_diagnostics();
        refresh_battery_debug();
        return;
    }
    lv_label_set_text_fmt(battery_label, "%s %u%%%s",
                          status.percent < 25 ? LV_SYMBOL_BATTERY_EMPTY : LV_SYMBOL_BATTERY_FULL,
                          status.percent,
                          status.charge_state == BOARD_BATTERY_CHARGING ? " " LV_SYMBOL_CHARGE : "");
    lv_obj_set_style_text_color(battery_label,
        status.charge_state == BOARD_BATTERY_CHARGING ? ui_kit_tone_color(UI_TONE_SUCCESS) : ui_kit_tone_color(UI_TONE_MUTED),
        LV_PART_MAIN);
    if (status.charge_state != BOARD_BATTERY_CHARGING && status.percent <= 15) {
        lv_obj_set_style_text_color(battery_label, ui_kit_tone_color(UI_TONE_DANGER),
        LV_PART_MAIN);
    }
    refresh_diagnostics();
    refresh_battery_debug();
}

static lv_obj_t *add_nav_row(const char *title, const char *value, ui_tone_t tone,
                             bool enabled, lv_event_cb_t callback, void *user_data)
{
    const ui_row_spec_t spec = {
        .title = title,
        .subtitle = NULL,
        .value = value,
        .tone = tone,
        .enabled = enabled,
    };
    return ui_kit_create_row(page_body, navigation_group, &spec, callback, user_data);
}

static lv_obj_t *add_nav_card(const char *title, const char *subtitle,
                              const char *value, ui_tone_t tone,
                              lv_event_cb_t callback, void *user_data)
{
    const ui_row_spec_t spec = {
        .title = title,
        .subtitle = subtitle,
        .value = value,
        .tone = tone,
        .enabled = true,
    };
    return ui_kit_create_row(page_body, navigation_group, &spec, callback, user_data);
}

static lv_obj_t *add_nav_wrapped_card(const char *title, const char *subtitle,
                                      const char *value, ui_tone_t tone,
                                      lv_event_cb_t callback, void *user_data)
{
    const ui_row_spec_t spec = {
        .title = title,
        .subtitle = subtitle,
        .value = value,
        .tone = tone,
        .enabled = true,
    };
    return ui_kit_create_wrapped_row(page_body, navigation_group, &spec, callback, user_data);
}

static lv_obj_t *add_nav_button(const char *text, lv_event_cb_t callback, void *user_data)
{
    return add_nav_row(text, NULL, UI_TONE_DEFAULT, true, callback, user_data);
}

/* Service actions are intentionally not bound to clicks: touch and EC11 must
 * hold the selected row before a command is sent to Moonraker. */
static lv_obj_t *add_nav_hold_row(const char *title, const char *value, ui_tone_t tone,
                                  lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *row = add_nav_row(title, value, tone, true, NULL, NULL);
    /* HOLD is deliberately short: give the action name the unused space so
     * labels such as "Firmware restart" stay fully readable on 320 px. */
    if (row != NULL) {
        lv_obj_t *title_label = lv_obj_get_child(row, 0);
        lv_obj_t *value_label = lv_obj_get_child(row, 1);
        if (title_label != NULL) lv_obj_set_width(title_label, LV_PCT(70));
        if (value_label != NULL) lv_obj_set_width(value_label, LV_PCT(27));
    }
    if (row != NULL && callback != NULL) {
        lv_obj_add_event_cb(row, callback, LV_EVENT_LONG_PRESSED, user_data);
    }
    return row;
}

/* Long macro names are user-provided and must remain readable.  A wrapped
 * card also avoids the trailing HOLD affordance taking space from the name. */
static lv_obj_t *add_nav_wrapped_hold_card(const char *title, ui_tone_t tone,
                                           lv_event_cb_t callback, void *user_data)
{
    const ui_row_spec_t spec = {
        .title = title,
        .subtitle = NULL,
        .value = NULL,
        .tone = tone,
        .enabled = true,
    };
    lv_obj_t *row = ui_kit_create_wrapped_row(page_body, navigation_group, &spec, NULL, NULL);
    if (row != NULL && callback != NULL) {
        lv_obj_add_event_cb(row, callback, LV_EVENT_LONG_PRESSED, user_data);
    }
    return row;
}

static void add_print_action(lv_obj_t *parent, lv_align_t align, const char *label_text,
                             ui_tone_t tone, lv_event_cb_t callback)
{
    lv_obj_t *action = lv_btn_create(parent);
    lv_obj_set_size(action, 146, 42);
    lv_obj_align(action, align, 0, 0);
    lv_obj_set_style_bg_color(action, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_color(action, ui_kit_tone_color(tone), LV_PART_MAIN);
    lv_obj_set_style_border_width(action, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(action, ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                  LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(action, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(action, ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(action, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_radius(action, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(action, touch_focus_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(action, callback, LV_EVENT_LONG_PRESSED, NULL);
    lv_group_add_obj(navigation_group, action);

    lv_obj_t *label = lv_label_create(action);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, ui_kit_tone_color(tone), LV_PART_MAIN);
    lv_obj_center(label);
}

static void refresh_pause_action(const printer_state_t *printer)
{
    if (monitor_pause_action == NULL || monitor_pause_action_label == NULL) return;

    const bool paused = printer->job_state == PRINTER_JOB_PAUSED;
    const ui_tone_t tone = paused ? UI_TONE_SUCCESS : UI_TONE_ACCENT;
    lv_label_set_text(monitor_pause_action_label,
                      paused ? "Resume print" : "Pause print");
    lv_obj_set_style_border_color(monitor_pause_action, ui_kit_tone_color(tone), LV_PART_MAIN);
    lv_obj_set_style_text_color(monitor_pause_action_label, ui_kit_tone_color(tone), LV_PART_MAIN);
    lv_obj_center(monitor_pause_action_label);
}

/* Keep both print actions in the visible monitor viewport.  Vertical action
 * rows put Cancel below the bottom navigation on the 320 x 480 display. */
static void add_print_actions(const printer_state_t *printer)
{
    lv_obj_t *actions = lv_obj_create(page_body);
    lv_obj_set_size(actions, LV_PCT(100), 42);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(actions, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(actions, 0, LV_PART_MAIN);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    const bool paused = printer->job_state == PRINTER_JOB_PAUSED;
    add_print_action(actions, LV_ALIGN_LEFT_MID,
                     paused ? "Resume print" : "Pause print",
                     paused ? UI_TONE_SUCCESS : UI_TONE_ACCENT, toggle_pause_print_event);
    monitor_pause_action = lv_obj_get_child(actions, 0);
    monitor_pause_action_label = monitor_pause_action == NULL ? NULL :
                                 lv_obj_get_child(monitor_pause_action, 0);
    add_print_action(actions, LV_ALIGN_RIGHT_MID, "Cancel print", UI_TONE_DANGER,
                     cancel_print_event);
}

static void clear_page(void)
{
    lv_group_remove_all_objs(navigation_group);
    /* The page container survives navigation.  Its scroll offset does not,
     * otherwise a short Monitor page inherits the previous page's viewport. */
    lv_obj_scroll_to(page_body, 0, 0, LV_ANIM_OFF);
    diagnostics_label = NULL;
    fleet_printer_button = NULL;
    status_connection_button = NULL;
    status_state_button = NULL;
    status_job_button = NULL;
    status_hotend_button = NULL;
    status_bed_button = NULL;
    if (monitor_state_label != NULL) lv_anim_del(monitor_state_label, NULL);
    monitor_state_label = NULL;
    monitor_status_pulsing = false;
    monitor_job_label = NULL;
    monitor_progress_label = NULL;
    monitor_progress_bar = NULL;
    monitor_total_time_label = NULL;
    monitor_eta_label = NULL;
    monitor_hotend_label = NULL;
    monitor_bed_label = NULL;
    monitor_position_label = NULL;
    monitor_pause_action = NULL;
    console_output = NULL;
    console_output_label = NULL;
    console_new_rows = NULL;
    monitor_pause_action_label = NULL;
    move_value_row = NULL;
    memset(move_axis_rows, 0, sizeof(move_axis_rows));
    move_position_section = NULL;
    move_jog_section = NULL;
    move_extrusion_section = NULL;
    move_extruder_feed_row = NULL;
    move_extruder_speed_row = NULL;
    move_step_row = NULL;
    move_park_row = NULL;
    button_mapping_row = NULL;
    tune_value_row = NULL;
    tune_offset_row = NULL;
    tune_speed_row = NULL;
    tune_flow_row = NULL;
    memset(tune_fan_rows, 0, sizeof(tune_fan_rows));
    memset(tune_heater_rows, 0, sizeof(tune_heater_rows));
    lv_obj_clean(page_body);
}

static void navigation_event(lv_event_t *event)
{
    if (ota_update_is_installing()) return;
    navigate_to((page_t)(uintptr_t)lv_event_get_user_data(event));
}

/* This control belongs to the screen rather than page_body.  Consequently it
 * remains at the top while a long file list is scrolled. */
static void update_files_pagination(void)
{
    if (files_pagination == NULL) return;

    const bool show = !file_browser_snapshot.loading && file_browser_snapshot.valid &&
                      file_browser_snapshot.entry_count > MOONRAKER_FILE_BROWSER_PAGE_SIZE;
    if (!show) {
        lv_obj_add_flag(files_pagination, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const size_t current_page = files_page_offset / MOONRAKER_FILE_BROWSER_PAGE_SIZE + 1;
    const size_t page_count = (file_browser_snapshot.entry_count + MOONRAKER_FILE_BROWSER_PAGE_SIZE - 1) /
                              MOONRAKER_FILE_BROWSER_PAGE_SIZE;
    const bool has_previous = files_page_offset > 0;
    const bool has_next = files_page_offset + MOONRAKER_FILE_BROWSER_PAGE_SIZE <
                          file_browser_snapshot.entry_count;

    lv_label_set_text_fmt(files_page_label, "%u/%u", (unsigned)current_page, (unsigned)page_count);
    lv_obj_clear_flag(files_pagination, LV_OBJ_FLAG_HIDDEN);

    if (has_previous) {
        lv_obj_clear_state(files_previous_page_button, LV_STATE_DISABLED);
        lv_group_add_obj(navigation_group, files_previous_page_button);
    } else {
        lv_obj_add_state(files_previous_page_button, LV_STATE_DISABLED);
    }
    if (has_next) {
        lv_obj_clear_state(files_next_page_button, LV_STATE_DISABLED);
        lv_group_add_obj(navigation_group, files_next_page_button);
    } else {
        lv_obj_add_state(files_next_page_button, LV_STATE_DISABLED);
    }
}

/* History uses the same fixed page controls as Files.  Keeping them outside
 * page_body means the current page and both arrows remain reachable while a
 * list of long file names is scrolled. */
static void update_history_pagination(void)
{
    if (files_pagination == NULL) return;

    /* Keep this control visible even for a single History page: unlike a
     * footer row it makes paging discoverable and gives the screen the same
     * fixed navigation landmark as Files. */
    const bool show = !history_snapshot.loading && history_snapshot.valid &&
                      history_snapshot.count > 0;
    if (!show) {
        lv_obj_add_flag(files_pagination, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const size_t total = history_snapshot.total < history_snapshot.count ?
        history_snapshot.count : history_snapshot.total;
    const size_t current_page = history_snapshot.offset / MOONRAKER_HISTORY_PAGE_SIZE + 1;
    const size_t page_count = (total + MOONRAKER_HISTORY_PAGE_SIZE - 1) /
                              MOONRAKER_HISTORY_PAGE_SIZE;
    const bool has_previous = history_snapshot.offset > 0;
    const bool has_next = history_snapshot.offset + history_snapshot.count < total;

    lv_label_set_text_fmt(files_page_label, "%u/%u", (unsigned)current_page, (unsigned)page_count);
    lv_obj_clear_flag(files_pagination, LV_OBJ_FLAG_HIDDEN);

    if (has_previous) {
        lv_obj_clear_state(files_previous_page_button, LV_STATE_DISABLED);
        lv_group_add_obj(navigation_group, files_previous_page_button);
    } else {
        lv_obj_add_state(files_previous_page_button, LV_STATE_DISABLED);
    }
    if (has_next) {
        lv_obj_clear_state(files_next_page_button, LV_STATE_DISABLED);
        lv_group_add_obj(navigation_group, files_next_page_button);
    } else {
        lv_obj_add_state(files_next_page_button, LV_STATE_DISABLED);
    }
}

static const int32_t move_steps_centi_mm[] = { 10, 100, 1000, 5000 };
static const char *const move_park_labels[] = { "ALL", "X", "Y", "Z" };
static const char *const move_axis_labels[] = { "X", "Y", "Z" };

static bool move_commands_available(void)
{
    printer_state_t printer;
    printer_get_state(&printer);
    return printer.connection == PRINTER_CONNECTION_ONLINE &&
           printer.operational_state == PRINTER_STATE_READY;
}

static void format_move_distance(char *text, size_t text_size, int32_t centi_mm)
{
    const char sign = centi_mm < 0 ? '-' : '+';
    const uint32_t magnitude = (uint32_t)(centi_mm < 0 ? -centi_mm : centi_mm);
    snprintf(text, text_size, "%c%lu.%02lu mm", sign,
             (unsigned long)(magnitude / 100U), (unsigned long)(magnitude % 100U));
}

static void format_coordinate(char *text, size_t text_size, int32_t centi_mm)
{
    const char *sign = centi_mm < 0 ? "-" : "";
    const uint32_t magnitude = (uint32_t)(centi_mm < 0 ? -centi_mm : centi_mm);
    snprintf(text, text_size, "%s%lu.%02lu mm", sign,
             (unsigned long)(magnitude / 100U), (unsigned long)(magnitude % 100U));
}

static void format_axis_title(char *text, size_t text_size, const printer_state_t *printer,
                              uint8_t axis)
{
    char coordinate[20];
    if (printer->toolhead_position_valid) {
        const int32_t positions[] = { printer->toolhead_x_centi_mm,
                                      printer->toolhead_y_centi_mm,
                                      printer->toolhead_z_centi_mm };
        format_coordinate(coordinate, sizeof(coordinate), positions[axis]);
    } else {
        strlcpy(coordinate, "--", sizeof(coordinate));
    }
    snprintf(text, text_size, "#%s %s#  %s",
             axis_homing_color(printer->toolhead_axes_homed[axis]),
             move_axis_labels[axis], coordinate);
}

static void refresh_move_rows(const printer_state_t *printer)
{
    if (current_page != PAGE_MOVE) return;
    for (uint8_t axis = 0; axis < 3; ++axis) {
        if (move_axis_rows[axis] == NULL) continue;
        char title[32];
        format_axis_title(title, sizeof(title), printer, axis);
        if (move_mode == MOVE_MODE_AXIS && axis == move_axis) {
            char value[24];
            format_move_distance(value, sizeof(value), move_distance_centi_mm);
            ui_kit_set_row_text(move_axis_rows[axis], title, value);
        } else {
            ui_kit_set_row_text(move_axis_rows[axis], title, "");
        }
        lv_obj_t *title_label = lv_obj_get_child(move_axis_rows[axis], 0);
        if (title_label != NULL) lv_label_set_recolor(title_label, true);
    }
    if (move_extruder_feed_row != NULL) {
        char value[24];
        if (move_mode == MOVE_MODE_EXTRUDER) {
            format_move_distance(value, sizeof(value), move_distance_centi_mm);
            ui_kit_set_row_text(move_extruder_feed_row, "Extruder feed", value);
        } else {
            ui_kit_set_row_text(move_extruder_feed_row, "Extruder feed", "");
        }
    }
    if (move_extruder_speed_row != NULL) {
        char value[24];
        snprintf(value, sizeof(value), "%ld mm/s", (long)move_extruder_speed_mm_s);
        ui_kit_set_row_text(move_extruder_speed_row, "Extruder speed", value);
    }
}

static void refresh_tune_rows(const printer_state_t *printer)
{
    if (current_page != PAGE_TUNE || tune_mode != TUNE_MODE_BROWSE) return;
    /* Capabilities are synthesized from the current printer state by the
     * compatibility source, so obtain a fresh snapshot for live temperatures
     * and fan speed instead of rebuilding the page. */
    (void)printer_get_capabilities(&tune_capabilities);
    if (printer->tune_values_valid) {
        tune_offset_centi_mm = printer->z_offset_centi_mm;
        tune_speed_percent = printer->speed_percent;
        tune_flow_percent = printer->flow_percent;
    }

    char value[28];
    if (tune_offset_row != NULL) {
        const char sign = tune_offset_centi_mm < 0 ? '-' : '+';
        const uint32_t magnitude = (uint32_t)(tune_offset_centi_mm < 0 ?
            -tune_offset_centi_mm : tune_offset_centi_mm);
        snprintf(value, sizeof(value), "%c%lu.%02lu mm", sign,
                 (unsigned long)(magnitude / 100U), (unsigned long)(magnitude % 100U));
        ui_kit_set_row_text(tune_offset_row, "Z Offset", value);
    }
    if (tune_speed_row != NULL) {
        snprintf(value, sizeof(value), "%ld%%", (long)tune_speed_percent);
        ui_kit_set_row_text(tune_speed_row, "Speed", value);
    }
    if (tune_flow_row != NULL) {
        snprintf(value, sizeof(value), "%ld%%", (long)tune_flow_percent);
        ui_kit_set_row_text(tune_flow_row, "Flow", value);
    }
    for (size_t i = 0; i < tune_capabilities.fan_count; ++i) {
        if (tune_fan_rows[i] == NULL) continue;
        snprintf(value, sizeof(value), "%u%%", tune_capabilities.fans[i].speed_percent);
        ui_kit_set_row_text(tune_fan_rows[i], tune_capabilities.fans[i].label, value);
    }
    for (size_t i = 0; i < tune_capabilities.heater_count; ++i) {
        if (tune_heater_rows[i] == NULL) continue;
        const printer_heater_capability_t *heater = &tune_capabilities.heaters[i];
        snprintf(value, sizeof(value), "%d.%d / %d.%d C",
                 heater->current_deci_c / 10, abs(heater->current_deci_c % 10),
                 heater->target_deci_c / 10, abs(heater->target_deci_c % 10));
        ui_kit_set_row_text(tune_heater_rows[i], heater->label, value);
    }
}

static void move_exit_editing(void)
{
    lv_obj_t *edited_row = move_value_row;
    move_mode = MOVE_MODE_BROWSE;
    move_distance_centi_mm = 0;
    move_last_rotate_us = 0;
    if (move_position_section != NULL) lv_label_set_text(move_position_section, "POSITION (mm)");
    if (move_jog_section != NULL) lv_label_set_text(move_jog_section, "JOG");
    if (move_extrusion_section != NULL) lv_label_set_text(move_extrusion_section, "EXTRUSION");
    if (edited_row != NULL) {
        /* Restore the row's normal semantic value color; edit mode must not
         * leave an amber/brown warning value behind. */
        ui_kit_set_row_tone(edited_row, edited_row == move_park_row ?
                            UI_TONE_WARNING : UI_TONE_ACCENT);
        set_inline_row_visual(edited_row, false);
    }
    if (move_step_row != NULL) {
        char step[24];
        const int32_t selected_step = move_steps_centi_mm[move_step_index];
        snprintf(step, sizeof(step), "%ld.%02ld mm", (long)(selected_step / 100),
                 (long)(selected_step % 100));
        ui_kit_set_row_text(move_step_row, "Step", step);
    }
    if (move_park_row != NULL) ui_kit_set_row_text(move_park_row, "Park", "ALL");
    printer_state_t printer;
    if (printer_get_state(&printer) == ESP_OK) refresh_move_rows(&printer);
    move_value_row = NULL;
}

static void move_update_value_row(void)
{
    if (move_value_row == NULL) return;
    char value[24];
    lv_obj_t *title_label = lv_obj_get_child(move_value_row, 0);
    const char *title = title_label == NULL ? "" : lv_label_get_text(title_label);
    if (move_mode == MOVE_MODE_AXIS || move_mode == MOVE_MODE_EXTRUDER) {
        format_move_distance(value, sizeof(value), move_distance_centi_mm);
        ui_kit_set_row_text(move_value_row, title, value);
    } else if (move_mode == MOVE_MODE_EXTRUDER_SPEED) {
        snprintf(value, sizeof(value), "%ld mm/s", (long)move_extruder_speed_mm_s);
        ui_kit_set_row_text(move_value_row, "Extruder speed", value);
    } else if (move_mode == MOVE_MODE_STEP) {
        const int32_t step = move_steps_centi_mm[move_step_index];
        snprintf(value, sizeof(value), "%ld.%02ld mm", (long)(step / 100),
                 (long)(step % 100));
        ui_kit_set_row_text(move_value_row, "Step", value);
    } else if (move_mode == MOVE_MODE_PARK) {
        ui_kit_set_row_text(move_value_row, "Park", move_park_labels[move_park_index]);
    }
}

static void move_axis_event(lv_event_t *event)
{
    move_axis = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    move_distance_centi_mm = 0;
    move_last_rotate_us = 0;
    move_mode = MOVE_MODE_AXIS;
    move_value_row = lv_event_get_target(event);
    set_inline_row_visual(move_value_row, true);
    move_update_value_row();
}

static void move_extruder_feed_event(lv_event_t *event)
{
    move_distance_centi_mm = 0;
    move_last_rotate_us = 0;
    move_mode = MOVE_MODE_EXTRUDER;
    move_value_row = lv_event_get_target(event);
    set_inline_row_visual(move_value_row, true);
    move_update_value_row();
}

static void move_extruder_speed_event(lv_event_t *event)
{
    move_mode = MOVE_MODE_EXTRUDER_SPEED;
    move_value_row = lv_event_get_target(event);
    set_inline_row_visual(move_value_row, true);
    move_update_value_row();
}

static void move_step_event(lv_event_t *event)
{
    (void)event;
    move_mode = MOVE_MODE_STEP;
    move_value_row = lv_event_get_target(event);
    set_inline_row_visual(move_value_row, true);
    move_update_value_row();
}

static void move_park_event(lv_event_t *event)
{
    (void)event;
    move_park_index = 0;
    move_mode = MOVE_MODE_PARK;
    move_value_row = lv_event_get_target(event);
    set_inline_row_visual(move_value_row, true);
    move_update_value_row();
}

static void move_cancel_event(lv_event_t *event)
{
    (void)event;
    move_exit_editing();
}

static void move_confirm_event(lv_event_t *event)
{
    (void)event;
    char script[160];
    if (move_mode == MOVE_MODE_AXIS && move_distance_centi_mm != 0) {
        const char sign = move_distance_centi_mm < 0 ? '-' : '+';
        const uint32_t magnitude = (uint32_t)(move_distance_centi_mm < 0 ?
            -move_distance_centi_mm : move_distance_centi_mm);
        snprintf(script, sizeof(script),
                 "SAVE_GCODE_STATE NAME=PENDANT_MOVE\nG91\nG1 %s%c%lu.%02lu F3000\nRESTORE_GCODE_STATE NAME=PENDANT_MOVE",
                 move_axis_labels[move_axis], sign, (unsigned long)(magnitude / 100U),
                 (unsigned long)(magnitude % 100U));
        report_command_result("Move", moonraker_send_gcode(script));
    } else if (move_mode == MOVE_MODE_EXTRUDER && move_distance_centi_mm != 0) {
        const char sign = move_distance_centi_mm < 0 ? '-' : '+';
        const uint32_t magnitude = (uint32_t)(move_distance_centi_mm < 0 ?
            -move_distance_centi_mm : move_distance_centi_mm);
        snprintf(script, sizeof(script),
                 "SAVE_GCODE_STATE NAME=PENDANT_EXTRUDE\nM83\nG1 E%c%lu.%02lu F%ld\n"
                 "RESTORE_GCODE_STATE NAME=PENDANT_EXTRUDE",
                 sign, (unsigned long)(magnitude / 100U), (unsigned long)(magnitude % 100U),
                 (long)move_extruder_speed_mm_s * 60L);
        report_command_result(move_distance_centi_mm < 0 ? "Retract" : "Extrude",
                              moonraker_send_gcode(script));
    } else if (move_mode == MOVE_MODE_PARK) {
        if (move_park_index == 0) strlcpy(script, "G28", sizeof(script));
        else snprintf(script, sizeof(script), "G28 %s", move_park_labels[move_park_index]);
        report_command_result("Home", moonraker_send_gcode(script));
    }
    /* Step selection only changes local UI state; hold commits the edit. */
    move_exit_editing();
}

static void editor_close(void)
{
    if (editor_overlay != NULL) {
        lv_obj_del(editor_overlay);
        editor_overlay = NULL;
        editor_textarea = NULL;
        editor_error_label = NULL;
    }
}

static void editor_save_event(lv_event_t *event)
{
    (void)event;
    const char *value = lv_textarea_get_text(editor_textarea);
    switch (editor_field) {
    case EDIT_WIFI_SSID: strlcpy(editable_wifi.ssid, value, sizeof(editable_wifi.ssid)); break;
    case EDIT_WIFI_PASSWORD: strlcpy(editable_wifi.password, value, sizeof(editable_wifi.password)); break;
    case EDIT_PRINTER_NAME: strlcpy(editable_printer.name, value, sizeof(editable_printer.name)); break;
    case EDIT_PRINTER_HOST: strlcpy(editable_printer.host, value, sizeof(editable_printer.host)); break;
    case EDIT_PRINTER_PORT: {
        char *end = NULL;
        unsigned long port = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || port == 0 || port > UINT16_MAX) {
            lv_label_set_text(editor_error_label, "Enter a port from 1 to 65535");
            return;
        }
        editable_printer.port = (uint16_t)port;
        break;
    }
    case EDIT_CONSOLE_COMMAND:
        strlcpy(console_command, value, sizeof(console_command));
        break;
    case EDIT_LEFT_BUTTON_GCODE:
    case EDIT_RIGHT_BUTTON_GCODE:
        if (settings_get_button_mapping(&editable_button_mapping) != ESP_OK) return;
        strlcpy(editor_field == EDIT_LEFT_BUTTON_GCODE ? editable_button_mapping.left_gcode :
                                                        editable_button_mapping.right_gcode,
                value, SETTINGS_BUTTON_GCODE_MAX_LEN + 1);
        if (settings_set_button_mapping(&editable_button_mapping) != ESP_OK) {
            lv_label_set_text(editor_error_label, "Keep one button assigned to Back");
            return;
        }
        break;
    }
    editor_close();
    navigate_to(current_page);
}

static void editor_textarea_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) editor_save_event(NULL);
}

static void hide_keyboard_close_key(lv_obj_t *keyboard)
{
    /* The stock LVGL layout puts a keyboard icon here to hide the keyboard.
     * This editor is modal, so Back is the single, consistent cancel action.
     * Find it by label: custom Console maps intentionally use that slot for
     * cursor navigation and do not share the stock button indices. */
    const char **map = lv_keyboard_get_map_array(keyboard);
    uint16_t key = 0;
    for (size_t item = 0; map != NULL && map[item][0] != '\0'; ++item) {
        if (strcmp(map[item], "\n") == 0) continue;
        if (strcmp(map[item], LV_SYMBOL_KEYBOARD) == 0) {
            lv_btnmatrix_set_btn_ctrl(keyboard, key, LV_BTNMATRIX_CTRL_HIDDEN);
        }
        ++key;
    }
}

static void keyboard_layout_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        if (editor_field != EDIT_CONSOLE_COMMAND && editor_field != EDIT_LEFT_BUTTON_GCODE &&
            editor_field != EDIT_RIGHT_BUTTON_GCODE) hide_keyboard_close_key(lv_event_get_target(event));
    }
}

static void theme_event(lv_event_t *event)
{
    (void)event;
    settings_theme_t theme;
    settings_get_theme(&theme);
    theme = theme == SETTINGS_THEME_LIGHT ? SETTINGS_THEME_DARK : SETTINGS_THEME_LIGHT;
    if (settings_set_theme(theme) != ESP_OK) return;
    ui_kit_set_theme(theme == SETTINGS_THEME_DARK ? UI_THEME_DARK : UI_THEME_LIGHT);
    ui_kit_apply_screen(lv_scr_act());
    lv_obj_set_style_text_color(page_title, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
    lv_obj_set_style_text_color(back_hint, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_color(header_printer_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
    /* Pagination lives outside page_body and is not recreated on a theme
     * change. Refresh its explicit styles as well, otherwise its light text
     * from the dark theme disappears against the light screen background. */
    lv_obj_set_style_text_color(files_page_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
    lv_obj_t *page_buttons[] = { files_previous_page_button, files_next_page_button };
    for (size_t i = 0; i < sizeof(page_buttons) / sizeof(page_buttons[0]); ++i) {
        lv_obj_set_style_bg_color(page_buttons[i], ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_color(page_buttons[i], ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                      LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(page_buttons[i], ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                       LV_PART_MAIN | LV_STATE_FOCUSED);
    }
    navigate_to(PAGE_SETTINGS);
}

static const char *button_action_text(settings_button_action_t action)
{
    switch (action) {
    case SETTINGS_BUTTON_ACTION_BACK: return "Back";
    case SETTINGS_BUTTON_ACTION_ESTOP: return "E-stop";
    case SETTINGS_BUTTON_ACTION_GCODE: return "G-code";
    case SETTINGS_BUTTON_ACTION_NONE:
    default: return "None";
    }
}

static void button_mapping_event(lv_event_t *event)
{
    const bool left = (bool)(uintptr_t)lv_event_get_user_data(event);
    if (settings_get_button_mapping(&editable_button_mapping) != ESP_OK) return;
    button_mapping_editing = true;
    button_mapping_edit_left = left;
    button_mapping_original_action = left ? editable_button_mapping.left : editable_button_mapping.right;
    button_mapping_row = lv_event_get_target(event);
    set_inline_row_visual(button_mapping_row, true);
    lv_label_set_text(back_hint, "Rotate: choose  |  Press: save  |  Back: cancel");
}

static void emergency_stop_event(lv_event_t *event)
{
    (void)event;
    report_command_result("E-STOP", moonraker_emergency_stop());
}

static void firmware_check_event(lv_event_t *event)
{
    (void)event;
    (void)ota_update_check();
    navigate_to(PAGE_FIRMWARE);
}

static void firmware_install_release_event(lv_event_t *event)
{
    const uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    (void)ota_update_install_release(index);
    navigate_to(PAGE_FIRMWARE);
}

static void open_editor(edit_field_t field)
{
    static char port[6];
    const char *title = "Edit";
    const char *value = "";
    const char *hint = "";
    uint16_t max_length = 63;
    bool password = false;
    editor_field = field;
    switch (field) {
    case EDIT_WIFI_SSID:
        title = "Wi-Fi name"; value = editable_wifi.ssid; hint = "Network name (SSID)"; max_length = SETTINGS_WIFI_SSID_MAX_LEN; break;
    case EDIT_WIFI_PASSWORD:
        title = "Wi-Fi password"; value = editable_wifi.password; hint = "Stored locally in NVS";
        password = true; max_length = SETTINGS_WIFI_PASSWORD_MAX_LEN; break;
    case EDIT_PRINTER_NAME:
        title = "Printer name"; value = editable_printer.name; hint = "Shown in Fleet";
        max_length = SETTINGS_PRINTER_NAME_MAX_LEN; break;
    case EDIT_PRINTER_HOST:
        title = "Moonraker address"; value = editable_printer.host;
        hint = "IPv4, hostname, or URL"; max_length = SETTINGS_PRINTER_HOST_MAX_LEN; break;
    case EDIT_PRINTER_PORT:
        title = "Moonraker port";
        hint = "Usually 7125";
        snprintf(port, sizeof(port), "%u", editable_printer.port);
        value = port;
        max_length = 5;
        break;
    case EDIT_CONSOLE_COMMAND:
        title = "G-code command";
        value = console_command;
        hint = "Send requires HOLD; M112 uses emergency stop";
        max_length = sizeof(console_command) - 1;
        break;
    case EDIT_LEFT_BUTTON_GCODE:
    case EDIT_RIGHT_BUTTON_GCODE:
        if (settings_get_button_mapping(&editable_button_mapping) != ESP_OK) return;
        title = field == EDIT_LEFT_BUTTON_GCODE ? "Left button G-code" : "Right button G-code";
        value = field == EDIT_LEFT_BUTTON_GCODE ? editable_button_mapping.left_gcode :
                                                  editable_button_mapping.right_gcode;
        hint = "Runs when the button is held";
        max_length = SETTINGS_BUTTON_GCODE_MAX_LEN;
        break;
    }
    editor_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(editor_overlay, 320, 480);
    lv_obj_set_style_bg_color(editor_overlay, ui_kit_color(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(editor_overlay, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(editor_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(editor_overlay, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(editor_overlay, 6, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(editor_overlay);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, ui_kit_color(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    if (field != EDIT_CONSOLE_COMMAND && field != EDIT_LEFT_BUTTON_GCODE && field != EDIT_RIGHT_BUTTON_GCODE) {
        lv_obj_t *hint_label = lv_label_create(editor_overlay);
        lv_label_set_text(hint_label, hint);
        lv_obj_set_style_text_color(hint_label, ui_kit_color(UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    }
    editor_textarea = lv_textarea_create(editor_overlay);
    lv_obj_set_width(editor_textarea, LV_PCT(100));
    const bool multiline = field == EDIT_CONSOLE_COMMAND || field == EDIT_LEFT_BUTTON_GCODE ||
                           field == EDIT_RIGHT_BUTTON_GCODE;
    lv_obj_set_height(editor_textarea, multiline ? 76 : 46);
    lv_textarea_set_one_line(editor_textarea, !multiline);
    lv_textarea_set_cursor_click_pos(editor_textarea, true);
    lv_textarea_set_max_length(editor_textarea, max_length);
    lv_textarea_set_text(editor_textarea, value);
    lv_textarea_set_password_mode(editor_textarea, password);
    lv_obj_set_style_bg_color(editor_textarea, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_color(editor_textarea, ui_kit_color(UI_COLOR_BORDER_FOCUSED), LV_PART_MAIN);
    lv_obj_set_style_border_width(editor_textarea, 2, LV_PART_MAIN);
    lv_obj_set_style_outline_color(editor_textarea, ui_kit_color(UI_COLOR_BORDER_FOCUSED), LV_PART_MAIN);
    lv_obj_set_style_outline_width(editor_textarea, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(editor_textarea, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(editor_textarea, ui_kit_color(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_color(editor_textarea, ui_kit_color(UI_COLOR_TEXT_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    /* The keyboard retains focus for hardware navigation, so make the native
     * textarea caret explicit instead of relying on focus-only theme styling. */
    lv_obj_set_style_border_color(editor_textarea, ui_kit_color(UI_COLOR_BORDER_FOCUSED), LV_PART_CURSOR);
    lv_obj_set_style_border_width(editor_textarea, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(editor_textarea, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_pad_left(editor_textarea, -1, LV_PART_CURSOR);
    lv_obj_set_style_anim_time(editor_textarea, 400, LV_PART_CURSOR);
    lv_obj_add_event_cb(editor_textarea, editor_textarea_event, LV_EVENT_READY, NULL);
    editor_error_label = lv_label_create(editor_overlay);
    /* LVGL labels begin as \"Text\".  An error label must be empty until a
     * validation error actually occurs. */
    lv_label_set_text(editor_error_label, "");
    lv_obj_set_style_text_color(editor_error_label, ui_kit_tone_color(UI_TONE_DANGER), LV_PART_MAIN);
    lv_obj_t *keyboard = lv_keyboard_create(editor_overlay);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(keyboard, 320);
    lv_obj_set_height(keyboard, 220);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 12);
    lv_obj_set_style_bg_color(keyboard, ui_kit_color(UI_COLOR_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_border_color(keyboard, ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard, ui_kit_color(UI_COLOR_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, ui_kit_color(UI_COLOR_SURFACE_FOCUSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(keyboard, ui_kit_color(UI_COLOR_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, ui_kit_color(UI_COLOR_TEXT_PRIMARY), LV_PART_ITEMS);
    if (field == EDIT_PRINTER_PORT) lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    if (field == EDIT_CONSOLE_COMMAND || field == EDIT_LEFT_BUTTON_GCODE || field == EDIT_RIGHT_BUTTON_GCODE) {
        lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, console_kb_lower, console_kb_ctrl);
        lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, console_kb_upper, console_kb_ctrl);
        lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, console_kb_special, console_kb_ctrl);
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
    lv_keyboard_set_textarea(keyboard, editor_textarea);
    lv_obj_add_event_cb(keyboard, keyboard_layout_event, LV_EVENT_VALUE_CHANGED, NULL);
    if (field != EDIT_CONSOLE_COMMAND && field != EDIT_LEFT_BUTTON_GCODE && field != EDIT_RIGHT_BUTTON_GCODE)
        hide_keyboard_close_key(keyboard);
    lv_group_focus_obj(keyboard);
}

static void edit_field_event(lv_event_t *event)
{
    open_editor((edit_field_t)(uintptr_t)lv_event_get_user_data(event));
}

static void save_network_event(lv_event_t *event)
{
    (void)event;
    if (settings_set_wifi(&editable_wifi) == ESP_OK) wifi_manager_reconfigure();
    navigate_to(PAGE_NETWORK_SETUP);
}

static void scan_networks_event(lv_event_t *event)
{
    (void)event;
    if (wifi_manager_scan_start() == ESP_OK) {
        lv_timer_t *timer = lv_timer_create(scan_refresh_timer_cb, 2500, NULL);
        lv_timer_set_repeat_count(timer, 1);
    }
    navigate_to(PAGE_NETWORK_SETUP);
}

static void select_network_event(lv_event_t *event)
{
    wifi_manager_network_t networks[8];
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    size_t count = wifi_manager_get_networks(networks, 8);
    if (index < count) strlcpy(editable_wifi.ssid, networks[index].ssid, sizeof(editable_wifi.ssid));
    navigate_to(PAGE_NETWORK_SETUP);
}

static void save_printer_event(lv_event_t *event)
{
    (void)event;
    editable_printer.enabled = true;
    size_t index = editing_printer_index;
    esp_err_t result = index == SIZE_MAX ? settings_add_printer(&editable_printer, &index) :
                                          settings_set_printer_at(index, &editable_printer);
    if (result == ESP_OK) settings_set_active_printer_index(index);
    navigate_to(PAGE_PRINTER_LIST);
}

static void select_printer_complete_async(void *data)
{
    const uintptr_t result = (uintptr_t)data;
    printer_selection_pending = false;
    if (result != 0) navigate_to(PAGE_STATUS);
}

static void select_printer_task(void *context)
{
    const size_t index = (size_t)(uintptr_t)context;
    const bool saved = settings_set_active_printer_index(index) == ESP_OK;
    if (lvgl_port_lock(portMAX_DELAY)) {
        (void)lv_async_call(select_printer_complete_async, (void *)(uintptr_t)(saved ? 1U : 0U));
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void select_printer_event(lv_event_t *event)
{
    if (printer_selection_pending) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= SETTINGS_MAX_PRINTERS) return;
    printer_selection_pending = true;
    ui_kit_set_row_text(lv_event_get_target(event), printer_list_snapshot[index].name, "SAVING");
    if (xTaskCreate(select_printer_task, "select_printer", 3072,
                    (void *)(uintptr_t)index, 5, NULL) != pdPASS) {
        printer_selection_pending = false;
        navigate_to(PAGE_FLEET);
    }
}

static void edit_printer_event(lv_event_t *event)
{
    editing_printer_index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (settings_get_printer_at(editing_printer_index, &editable_printer) == ESP_OK) {
        navigate_to(PAGE_PRINTER_SETUP);
    }
}

static void add_printer_event(lv_event_t *event)
{
    (void)event;
    memset(&editable_printer, 0, sizeof(editable_printer));
    editable_printer.port = 7125;
    editing_printer_index = SIZE_MAX;
    navigate_to(PAGE_PRINTER_SETUP);
}

static const char *exclude_object_label(const moonraker_exclude_object_t *object)
{
    return object->name[0] ? object->name : "Unnamed object";
}

static void exclude_object_select_event(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (exclude_objects_snapshot == NULL || index >= exclude_objects_snapshot->count || exclude_objects_snapshot->objects[index].excluded) return;
    exclude_selected_index = (uint8_t)index;
    navigate_to(PAGE_EXCLUDE_CONFIRM);
}

static void exclude_objects_event(lv_event_t *event)
{
    (void)event;
    if (exclude_objects_snapshot == NULL) {
        exclude_objects_snapshot = heap_caps_calloc(1, sizeof(*exclude_objects_snapshot),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (exclude_objects_snapshot == NULL) {
        heap_caps_free(exclude_objects_snapshot); exclude_objects_snapshot = NULL;
        report_command_result("Open object map", ESP_ERR_NO_MEM); return;
    }
    moonraker_get_exclude_objects(exclude_objects_snapshot);
    if (!exclude_objects_snapshot->loading && !exclude_objects_snapshot->valid)
        (void)moonraker_request_exclude_objects();
    navigate_to(PAGE_EXCLUDE_OBJECTS);
}

static void exclude_object_confirm_event(lv_event_t *event)
{
    (void)event;
    if (exclude_objects_snapshot == NULL || exclude_selected_index >= exclude_objects_snapshot->count) return;
    char script[MOONRAKER_EXCLUDE_OBJECT_NAME_MAX_LEN + 32];
    /* Klipper's G-code parameter grammar treats object names as one token;
     * slicer markers conventionally use identifier-like names. */
    snprintf(script, sizeof(script), "EXCLUDE_OBJECT NAME=%s",
             exclude_objects_snapshot->objects[exclude_selected_index].name);
    const esp_err_t result = moonraker_send_gcode(script);
    if (result == ESP_OK) moonraker_mark_object_excluded(
        exclude_objects_snapshot->objects[exclude_selected_index].name);
    report_command_result("Exclude object", result);
    navigate_to(PAGE_EXCLUDE_OBJECTS);
}

static void add_exclude_map(void)
{
    if (exclude_objects_snapshot == NULL || exclude_objects_snapshot->count == 0) return;
    bool has_polygon = false;
    for (size_t i = 0; i < exclude_objects_snapshot->count; ++i) {
        if (exclude_objects_snapshot->objects[i].polygon_count >= 3) {
            has_polygon = true;
            break;
        }
    }
    /* The Moonraker fallback may have recovered names without the very large
     * polygon payload.  The list remains fully usable in that case. */
    if (!has_polygon) return;
    heap_caps_free(exclude_map_points);
    exclude_map_points = heap_caps_calloc(exclude_objects_snapshot->count *
                                           (MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX + 1),
                                           sizeof(*exclude_map_points), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (exclude_map_points == NULL) {
        add_nav_row("Not enough memory for object map", NULL, UI_TONE_DANGER, false, NULL, NULL);
        return;
    }
    lv_obj_t *map = lv_obj_create(page_body);
    lv_obj_set_size(map, LV_PCT(100), 300);
    lv_obj_set_style_bg_color(map, ui_kit_color(UI_COLOR_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_border_color(map, ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(map, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(map, 0, LV_PART_MAIN);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);
    typedef struct { lv_obj_t *hit; int16_t x, y; bool added; } map_selector_t;
    map_selector_t *selectors = heap_caps_calloc(exclude_objects_snapshot->count,
                                                  sizeof(*selectors),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    for (size_t i = 0; i < exclude_objects_snapshot->count; ++i) {
        const moonraker_exclude_object_t *o = &exclude_objects_snapshot->objects[i];
        if (o->polygon_count >= 3) for (uint8_t p = 0; p < o->polygon_count; ++p) {
            if (o->polygon[p][0] < min_x) min_x = o->polygon[p][0];
            if (o->polygon[p][0] > max_x) max_x = o->polygon[p][0];
            if (o->polygon[p][1] < min_y) min_y = o->polygon[p][1];
            if (o->polygon[p][1] > max_y) max_y = o->polygon[p][1];
        } else {
            if (o->center_x < min_x) min_x = o->center_x;
            if (o->center_x > max_x) max_x = o->center_x;
            if (o->center_y < min_y) min_y = o->center_y;
            if (o->center_y > max_y) max_y = o->center_y;
        }
    }
    const float range_x = (max_x - min_x) < 1.0f ? 1.0f : max_x - min_x;
    const float range_y = (max_y - min_y) < 1.0f ? 1.0f : max_y - min_y;
    const float scale_x = 260.0f / range_x;
    const float scale_y = 270.0f / range_y;
    const float scale = scale_x < scale_y ? scale_x : scale_y;
    for (size_t i = 0; i < exclude_objects_snapshot->count; ++i) {
        const moonraker_exclude_object_t *o = &exclude_objects_snapshot->objects[i];
        const float cx = o->center_x, cy = o->center_y;
        int16_t left = 300, top = 300, right = 0, bottom = 0;
        const uint8_t count = o->polygon_count >= 3 ? o->polygon_count : 1;
        for (uint8_t p = 0; p < count; ++p) {
            const float x = o->polygon_count >= 3 ? o->polygon[p][0] : cx;
            const float y = o->polygon_count >= 3 ? o->polygon[p][1] : cy;
            const int16_t px = (int16_t)(16 + (x - min_x) * scale);
            const int16_t py = (int16_t)(285 - (y - min_y) * scale);
            exclude_map_points[i * (MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX + 1) + p] = (lv_point_t){ px, py };
            if (px < left) left = px;
            if (px > right) right = px;
            if (py < top) top = py;
            if (py > bottom) bottom = py;
        }
        if (count == 1) { left -= 12; right += 12; top -= 12; bottom += 12; }
        if (o->polygon_count >= 3) {
            lv_point_t *points = &exclude_map_points[i * (MOONRAKER_EXCLUDE_POLYGON_POINTS_MAX + 1)];
            points[count] = points[0];
            lv_obj_t *line = lv_line_create(map);
            lv_line_set_points(line, points, count + 1);
            lv_obj_set_style_line_width(line, o->current ? 4 : 2, LV_PART_MAIN);
            lv_obj_set_style_line_color(line, ui_kit_tone_color(o->excluded ? UI_TONE_DANGER : o->current ? UI_TONE_ACCENT : UI_TONE_DEFAULT), LV_PART_MAIN);
        }
        lv_obj_t *hit = lv_btn_create(map);
        const int16_t hit_width = right - left + 8 < 40 ? 40 : right - left + 8;
        const int16_t hit_height = bottom - top + 8 < 40 ? 40 : bottom - top + 8;
        lv_obj_set_pos(hit, (left + right - hit_width) / 2,
                      (top + bottom - hit_height) / 2);
        lv_obj_set_size(hit, hit_width, hit_height);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(hit, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_outline_width(hit, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(hit, ui_kit_tone_color(UI_TONE_ACCENT),
                                       LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(hit, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
        if (!o->excluded) {
            lv_obj_add_event_cb(hit, exclude_object_select_event, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)i);
            if (selectors != NULL) selectors[i] = (map_selector_t){
                .hit = hit, .x = (int16_t)((left + right) / 2),
                .y = (int16_t)((top + bottom) / 2),
            };
            else lv_group_add_obj(navigation_group, hit);
        } else {
            lv_obj_add_state(hit, LV_STATE_DISABLED);
        }
        lv_obj_t *tag = lv_label_create(map); lv_label_set_text_fmt(tag, "%u", (unsigned)(i + 1));
        lv_obj_set_style_text_color(tag, ui_kit_tone_color(o->excluded ? UI_TONE_DANGER : o->current ? UI_TONE_ACCENT : UI_TONE_DEFAULT), LV_PART_MAIN);
        lv_obj_set_style_text_font(tag, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tag, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tag, 0, LV_PART_MAIN);
        lv_obj_align_to(tag, hit, LV_ALIGN_CENTER, 0, 0);
    }
    /* Klipper preserves slicer definition order, which is unrelated to the
     * visual layout.  Encoder traversal follows screen rows instead: select
     * the next top band, then walk that band from left to right. */
    if (selectors != NULL) {
        size_t remaining = 0;
        for (size_t i = 0; i < exclude_objects_snapshot->count; ++i)
            if (selectors[i].hit != NULL) ++remaining;
        while (remaining > 0) {
            int16_t row_top = INT16_MAX;
            for (size_t i = 0; i < exclude_objects_snapshot->count; ++i)
                if (selectors[i].hit != NULL && !selectors[i].added && selectors[i].y < row_top)
                    row_top = selectors[i].y;
            const int16_t row_bottom = row_top + 24;
            while (true) {
                size_t next = SIZE_MAX;
                for (size_t i = 0; i < exclude_objects_snapshot->count; ++i) {
                    if (selectors[i].hit == NULL || selectors[i].added ||
                        selectors[i].y > row_bottom) continue;
                    if (next == SIZE_MAX || selectors[i].x < selectors[next].x) next = i;
                }
                if (next == SIZE_MAX) break;
                lv_group_add_obj(navigation_group, selectors[next].hit);
                selectors[next].added = true;
                --remaining;
            }
        }
        heap_caps_free(selectors);
    }
}

static void add_exclude_confirmation(const moonraker_exclude_object_t *object)
{
    lv_obj_t *name = lv_label_create(page_body);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_height(name, LV_SIZE_CONTENT);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name, exclude_object_label(object));
    lv_obj_set_style_text_font(name, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);

    if (object->polygon_count >= 3) {
        lv_obj_t *preview = lv_obj_create(page_body);
        lv_obj_set_size(preview, LV_PCT(100), 170);
        lv_obj_set_style_bg_color(preview, ui_kit_color(UI_COLOR_SURFACE_2), LV_PART_MAIN);
        lv_obj_set_style_border_color(preview, ui_kit_tone_color(UI_TONE_WARNING), LV_PART_MAIN);
        lv_obj_set_style_border_width(preview, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
        lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

        float min_x = object->polygon[0][0], max_x = min_x;
        float min_y = object->polygon[0][1], max_y = min_y;
        for (uint8_t i = 1; i < object->polygon_count; ++i) {
            if (object->polygon[i][0] < min_x) min_x = object->polygon[i][0];
            if (object->polygon[i][0] > max_x) max_x = object->polygon[i][0];
            if (object->polygon[i][1] < min_y) min_y = object->polygon[i][1];
            if (object->polygon[i][1] > max_y) max_y = object->polygon[i][1];
        }
        const float range_x = max_x - min_x < 0.1f ? 0.1f : max_x - min_x;
        const float range_y = max_y - min_y < 0.1f ? 0.1f : max_y - min_y;
        const float scale_x = 250.0f / range_x;
        const float scale_y = 130.0f / range_y;
        const float scale = scale_x < scale_y ? scale_x : scale_y;
        const float offset_x = (300.0f - range_x * scale) * 0.5f;
        const float offset_y = (170.0f - range_y * scale) * 0.5f;

        heap_caps_free(exclude_map_points);
        exclude_map_points = heap_caps_calloc(object->polygon_count + 1,
                                               sizeof(*exclude_map_points),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (exclude_map_points != NULL) {
            for (uint8_t i = 0; i < object->polygon_count; ++i) {
                exclude_map_points[i].x = (lv_coord_t)(offset_x +
                    (object->polygon[i][0] - min_x) * scale);
                exclude_map_points[i].y = (lv_coord_t)(offset_y +
                    (max_y - object->polygon[i][1]) * scale);
            }
            exclude_map_points[object->polygon_count] = exclude_map_points[0];
            lv_obj_t *outline = lv_line_create(preview);
            lv_line_set_points(outline, exclude_map_points, object->polygon_count + 1);
            lv_obj_set_style_line_width(outline, 4, LV_PART_MAIN);
            lv_obj_set_style_line_rounded(outline, true, LV_PART_MAIN);
            lv_obj_set_style_line_color(outline, ui_kit_tone_color(UI_TONE_WARNING), LV_PART_MAIN);
        }
    }

    lv_obj_t *description = lv_label_create(page_body);
    lv_obj_set_width(description, LV_PCT(100));
    lv_obj_set_height(description, LV_SIZE_CONTENT);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_label_set_text(description, object->current ?
        "Currently printing. Future moves for this part will be skipped." :
        "Future print moves for this part will be skipped.");
    lv_obj_set_style_text_font(description, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(description, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);

    add_nav_hold_row("Exclude this object", "HOLD", UI_TONE_DANGER,
                     exclude_object_confirm_event, NULL);
}

static void tune_format_value(char *text, size_t text_size)
{
    if (tune_mode == TUNE_MODE_OFFSET) {
        const char sign = tune_value < 0 ? '-' : '+';
        const uint32_t magnitude = (uint32_t)(tune_value < 0 ? -tune_value : tune_value);
        snprintf(text, text_size, "%c%lu.%02lu mm", sign,
                 (unsigned long)(magnitude / 100U), (unsigned long)(magnitude % 100U));
    } else if (tune_mode == TUNE_MODE_HEATER) {
        snprintf(text, text_size, "%ld C", (long)(tune_value / 10));
    } else {
        snprintf(text, text_size, "%ld%%", (long)tune_value);
    }
}

static void tune_update_value_row(void)
{
    if (tune_value_row == NULL) return;
    lv_obj_t *title_label = lv_obj_get_child(tune_value_row, 0);
    char title[PRINTER_CAPABILITY_LABEL_MAX_LEN + 1];
    char value[28];
    strlcpy(title, title_label == NULL ? "Value" : lv_label_get_text(title_label), sizeof(title));
    tune_format_value(value, sizeof(value));
    ui_kit_set_row_text(tune_value_row, title, value);
}

static void tune_exit_editing(void)
{
    lv_obj_t *edited_row = tune_value_row;
    tune_mode = TUNE_MODE_BROWSE;
    tune_last_rotate_us = 0;
    if (edited_row != NULL) {
        ui_kit_set_row_tone(edited_row, UI_TONE_ACCENT);
        set_inline_row_visual(edited_row, false);
    }
    printer_state_t printer;
    if (printer_get_state(&printer) == ESP_OK) refresh_tune_rows(&printer);
    tune_value_row = NULL;
}

static void tune_value_event(lv_event_t *event)
{
    const uintptr_t encoded = (uintptr_t)lv_event_get_user_data(event);
    tune_mode = (tune_mode_t)(encoded >> 8);
    tune_item_index = (uint8_t)(encoded & 0xffU);
    tune_value_row = lv_event_get_target(event);
    tune_last_rotate_us = 0;
    switch (tune_mode) {
    case TUNE_MODE_OFFSET:
        tune_value = tune_offset_centi_mm;
        break;
    case TUNE_MODE_SPEED:
        tune_value = tune_speed_percent;
        break;
    case TUNE_MODE_FLOW:
        tune_value = tune_flow_percent;
        break;
    case TUNE_MODE_FAN:
        tune_value = tune_capabilities.fans[tune_item_index].speed_percent;
        break;
    case TUNE_MODE_HEATER:
        tune_value = tune_capabilities.heaters[tune_item_index].target_deci_c;
        break;
    default:
        return;
    }
    tune_original_value = tune_value;
    /* Keep the normal bright-orange value color while editing.  The explicit
     * frame, rather than a semantic warning color, communicates edit mode. */
    ui_kit_set_row_tone(tune_value_row, UI_TONE_ACCENT);
    set_inline_row_visual(tune_value_row, true);
    tune_update_value_row();
}

static void tune_cancel_event(lv_event_t *event)
{
    (void)event;
    if (tune_value_row != NULL) {
        tune_value = tune_original_value;
        tune_update_value_row();
    }
    tune_exit_editing();
}

static void tune_confirm_event(lv_event_t *event)
{
    (void)event;
    char script[160];
    switch (tune_mode) {
    case TUNE_MODE_OFFSET: {
        const int32_t adjustment = tune_value - tune_offset_centi_mm;
        if (adjustment != 0) {
            const char sign = adjustment < 0 ? '-' : '+';
            const uint32_t magnitude = (uint32_t)(adjustment < 0 ? -adjustment : adjustment);
            snprintf(script, sizeof(script), "SET_GCODE_OFFSET Z_ADJUST=%c%lu.%02lu MOVE=1", sign,
                     (unsigned long)(magnitude / 100U), (unsigned long)(magnitude % 100U));
            report_command_result("Z offset", moonraker_send_gcode(script));
        }
        tune_offset_centi_mm = tune_value;
        break;
    }
    case TUNE_MODE_SPEED:
        snprintf(script, sizeof(script), "M220 S%ld", (long)tune_value);
        report_command_result("Speed", moonraker_send_gcode(script));
        tune_speed_percent = tune_value;
        break;
    case TUNE_MODE_FLOW:
        snprintf(script, sizeof(script), "M221 S%ld", (long)tune_value);
        report_command_result("Flow", moonraker_send_gcode(script));
        tune_flow_percent = tune_value;
        break;
    case TUNE_MODE_FAN:
        /* The standard [fan] is controlled through M106 on a number of
         * Klipper configurations.  Keep SET_FAN_SPEED for named extra fans. */
        if (strcmp(tune_capabilities.fans[tune_item_index].id, "fan") == 0) {
            const int32_t pwm = (tune_value * 255 + 50) / 100;
            snprintf(script, sizeof(script), "M106 S%ld", (long)pwm);
        } else {
            snprintf(script, sizeof(script), "SET_FAN_SPEED FAN=%s SPEED=%ld.%02ld",
                     tune_capabilities.fans[tune_item_index].id, (long)(tune_value / 100),
                     (long)(tune_value % 100));
        }
        report_command_result("Fan", moonraker_send_gcode(script));
        tune_capabilities.fans[tune_item_index].speed_percent = (uint8_t)tune_value;
        break;
    case TUNE_MODE_HEATER:
        snprintf(script, sizeof(script), "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%ld",
                 tune_capabilities.heaters[tune_item_index].id, (long)(tune_value / 10));
        report_command_result("Temperature", moonraker_send_gcode(script));
        tune_capabilities.heaters[tune_item_index].target_deci_c = (int16_t)tune_value;
        break;
    default:
        break;
    }
    tune_exit_editing();
}

static void file_entry_event(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    moonraker_get_file_browser(&file_browser_snapshot);
    moonraker_file_entry_t entry;
    if (!moonraker_get_file_browser_entry(index, &entry)) return;
    int length = snprintf(selected_file_path, sizeof(selected_file_path), "%s/%s", file_browser_snapshot.path, entry.name);
    if (length <= 0 || length >= (int)sizeof(selected_file_path)) return;
    if (entry.is_directory) {
        files_page_offset = 0;
        moonraker_request_directory(selected_file_path);
        navigate_to(PAGE_FILES);
        return;
    }
    /* Moonraker's print API expects a path relative to the gcodes root. */
    memmove(selected_file_path, selected_file_path + strlen("gcodes/"),
            strlen(selected_file_path + strlen("gcodes/")) + 1);
    selected_file = entry;
    moonraker_request_file_metadata(selected_file_path);
    navigate_to(PAGE_FILE_DETAILS);
}

static void files_previous_page_event(lv_event_t *event)
{
    (void)event;
    if (current_page == PAGE_HISTORY) {
        const size_t offset = history_snapshot.offset >= MOONRAKER_HISTORY_PAGE_SIZE ?
            history_snapshot.offset - MOONRAKER_HISTORY_PAGE_SIZE : 0;
        (void)moonraker_request_history(offset);
        navigate_to(PAGE_HISTORY);
        return;
    }
    if (files_page_offset >= MOONRAKER_FILE_BROWSER_PAGE_SIZE) {
        files_page_offset -= MOONRAKER_FILE_BROWSER_PAGE_SIZE;
    }
    navigate_to(PAGE_FILES);
}

static void files_next_page_event(lv_event_t *event)
{
    (void)event;
    if (current_page == PAGE_HISTORY) {
        if (history_snapshot.offset + history_snapshot.count < history_snapshot.total) {
            (void)moonraker_request_history(history_snapshot.offset + MOONRAKER_HISTORY_PAGE_SIZE);
        }
        navigate_to(PAGE_HISTORY);
        return;
    }
    moonraker_get_file_browser(&file_browser_snapshot);
    if (files_page_offset + MOONRAKER_FILE_BROWSER_PAGE_SIZE < file_browser_snapshot.entry_count) {
        files_page_offset += MOONRAKER_FILE_BROWSER_PAGE_SIZE;
    }
    navigate_to(PAGE_FILES);
}

static void start_selected_file_event(lv_event_t *event)
{
    (void)event;
    printer_state_t printer;
    if (printer_get_state(&printer) != ESP_OK || !print_start_available(&printer)) {
        report_command_result("Start print", ESP_ERR_INVALID_STATE);
        navigate_to(PAGE_STATUS);
        return;
    }
    report_command_result("Start print", moonraker_print_start(selected_file_path));
    navigate_to(PAGE_STATUS);
}

static void pause_print_event(lv_event_t *event)
{
    (void)event;
    report_command_result("Pause", moonraker_print_pause());
}

static void resume_print_event(lv_event_t *event)
{
    (void)event;
    report_command_result("Resume", moonraker_print_resume());
}

static void toggle_pause_print_event(lv_event_t *event)
{
    printer_state_t printer;
    if (printer_get_state(&printer) != ESP_OK) {
        report_command_result("Pause/resume", ESP_FAIL);
        return;
    }
    if (printer.job_state == PRINTER_JOB_PAUSED) resume_print_event(event);
    else pause_print_event(event);
}

static void cancel_print_event(lv_event_t *event)
{
    (void)event;
    report_command_result("Cancel", moonraker_print_cancel());
}

static void service_gcode_event(lv_event_t *event)
{
    const char *script = lv_event_get_user_data(event);
    if (script != NULL) report_command_result("Service command", moonraker_send_gcode(script));
}

static void probe_gcode_event(lv_event_t *event)
{
    const char *script = lv_event_get_user_data(event);
    if (script == NULL) return;
    report_command_result("Probe calibration", moonraker_send_gcode(script));
}

static ui_tone_t prompt_style_tone(const char *style)
{
    if (strcmp(style, "error") == 0) return UI_TONE_DANGER;
    if (strcmp(style, "warning") == 0) return UI_TONE_WARNING;
    if (strcmp(style, "info") == 0 || strcmp(style, "primary") == 0) return UI_TONE_ACCENT;
    return UI_TONE_DEFAULT;
}

static void prompt_button_event(lv_event_t *event)
{
    moonraker_prompt_button_t *button = lv_event_get_user_data(event);
    if (button == NULL || button->command[0] == '\0') return;
    char command[MOONRAKER_PROMPT_COMMAND_MAX_LEN];
    strlcpy(command, button->command, sizeof(command));
    moonraker_dismiss_prompt();
    report_command_result("Prompt action", moonraker_send_gcode(command));
    navigate_to(prompt_return_page);
}

typedef enum {
    SERVICE_ACTION_MOONRAKER_RESTART,
    SERVICE_ACTION_SYSTEM_RESTART,
    SERVICE_ACTION_SYSTEM_SHUTDOWN,
} service_action_t;

static void service_action_event(lv_event_t *event)
{
    const service_action_t action = (service_action_t)(uintptr_t)lv_event_get_user_data(event);
    esp_err_t result = ESP_ERR_INVALID_ARG;
    const char *name = "System action";

    switch (action) {
    case SERVICE_ACTION_MOONRAKER_RESTART:
        name = "Moonraker restart";
        result = moonraker_restart_server();
        break;
    case SERVICE_ACTION_SYSTEM_RESTART:
        name = "System restart";
        result = moonraker_reboot_system();
        break;
    case SERVICE_ACTION_SYSTEM_SHUTDOWN:
        name = "System shutdown";
        result = moonraker_shutdown_system();
        break;
    }
    report_command_result(name, result);
}

static bool console_command_is_emergency_stop(const char *command)
{
    while (*command == ' ' || *command == '\t' || *command == '\r' || *command == '\n') ++command;
    return strncasecmp(command, "M112", 4) == 0 &&
           (command[4] == '\0' || command[4] == ' ' || command[4] == '\t' ||
            command[4] == '\r' || command[4] == '\n');
}

static void console_command_event(lv_event_t *event)
{
    (void)event;
    open_editor(EDIT_CONSOLE_COMMAND);
}

static void console_send_event(lv_event_t *event)
{
    (void)event;
    if (console_command[0] == '\0') return;
    const bool emergency_stop = console_command_is_emergency_stop(console_command);
    esp_err_t result;
    if (emergency_stop) {
        /* M112 must bypass Klipper's normal G-code queue. */
        moonraker_console_record_local(console_command);
        result = moonraker_emergency_stop();
    } else {
        result = moonraker_send_gcode(console_command);
    }
    report_command_result(emergency_stop ? "E-STOP" : "Console command", result);
    if (result == ESP_OK) console_command[0] = '\0';
    navigate_to(PAGE_CONSOLE);
}

static void console_new_rows_event(lv_event_t *event)
{
    (void)event;
    console_live = true;
    console_pending_count = 0;
    lv_label_set_text(page_title, "Console  LIVE");
    lv_obj_set_style_text_color(page_title, ui_kit_tone_color(UI_TONE_SUCCESS), LV_PART_MAIN);
    if (console_output != NULL) lv_obj_scroll_to_y(console_output, LV_COORD_MAX, LV_ANIM_OFF);
    refresh_console_output();
}

static void console_output_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE) {
        /* A deliberate touch scroll freezes the reader's position. */
        console_live = false;
        lv_label_set_text(page_title, "Console  PAUSED");
        lv_obj_set_style_text_color(page_title, ui_kit_tone_color(UI_TONE_WARNING), LV_PART_MAIN);
    }
}

static void refresh_console_output(void)
{
    if (console_output_label == NULL || console_snapshot == NULL || console_rendered_text == NULL) return;
    const size_t text_size = MOONRAKER_CONSOLE_LINES * (MOONRAKER_CONSOLE_LINE_MAX_LEN + 2) + 48;
    size_t used = 0;
    console_rendered_text[0] = '\0';
    if (console_snapshot->trimmed) {
        used = (size_t)snprintf(console_rendered_text, text_size,
                                "Earlier output trimmed\n");
    }
    for (size_t i = 0; i < console_snapshot->count && used < text_size; ++i) {
        const int written = snprintf(console_rendered_text + used, text_size - used,
                                     "%s\n", console_snapshot->lines[i]);
        if (written < 0) break;
        used += (size_t)written;
    }
    if (console_snapshot->count == 0) {
        strlcpy(console_rendered_text, console_snapshot->loading ? "Loading G-code history..." :
                (console_snapshot->error[0] ? console_snapshot->error : "No G-code output yet"), text_size);
    }
    lv_label_set_text(console_output_label, console_rendered_text);
    if (console_live) lv_obj_scroll_to_y(console_output, LV_COORD_MAX, LV_ANIM_OFF);
    if (console_new_rows != NULL) {
        if (console_live) lv_obj_add_flag(console_new_rows, LV_OBJ_FLAG_HIDDEN);
        else {
            char pending[24];
            snprintf(pending, sizeof(pending), "↓ %u new", (unsigned)console_pending_count);
            lv_label_set_text(lv_obj_get_child(console_new_rows, 0), pending);
            lv_obj_clear_flag(console_new_rows, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static bool console_buffers_acquire(void)
{
    const size_t text_size = MOONRAKER_CONSOLE_LINES * (MOONRAKER_CONSOLE_LINE_MAX_LEN + 2) + 48;
    if (console_snapshot == NULL) {
        console_snapshot = heap_caps_malloc(sizeof(*console_snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (console_rendered_text == NULL) {
        console_rendered_text = heap_caps_malloc(text_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (console_snapshot != NULL && console_rendered_text != NULL) return true;
    console_buffers_release();
    return false;
}

static void console_buffers_release(void)
{
    heap_caps_free(console_snapshot);
    heap_caps_free(console_rendered_text);
    console_snapshot = NULL;
    console_rendered_text = NULL;
}

static void macro_event(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index < macros_snapshot.count) {
        report_command_result(macros_snapshot.names[index], moonraker_send_gcode(macros_snapshot.names[index]));
    }
}

static void start_print_files_event(lv_event_t *event)
{
    (void)event;
    files_page_offset = 0;
    (void)moonraker_request_directory("gcodes");
    navigate_to(PAGE_FILES);
}

static void open_last_print_event(lv_event_t *event)
{
    (void)event;
    if (!last_print_job.file_exists || last_print_job.filename[0] == '\0') return;
    strlcpy(selected_file_path, last_print_job.filename, sizeof(selected_file_path));
    (void)moonraker_request_file_metadata(selected_file_path);
    navigate_to(PAGE_FILE_DETAILS);
}

static void history_entry_event(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= history_snapshot.count || !moonraker_get_history_job(index, &selected_history_job)) return;
    navigate_to(PAGE_HISTORY_DETAILS);
}

static void format_file_size(char *text, size_t text_size, uint32_t bytes)
{
    if (bytes >= 1024U * 1024U) snprintf(text, text_size, "%lu.%lu MB", (unsigned long)(bytes / (1024U * 1024U)),
                                         (unsigned long)((bytes * 10U / (1024U * 1024U)) % 10U));
    else snprintf(text, text_size, "%lu KB", (unsigned long)(bytes / 1024U));
}

static void format_file_details(char *text, size_t text_size,
                                uint32_t bytes, uint64_t modified_ms)
{
    char size[16];
    format_file_size(size, sizeof(size), bytes);
    if (modified_ms == 0) {
        snprintf(text, text_size, "%s", size);
        return;
    }

    const time_t modified_seconds = (time_t)(modified_ms / 1000U);
    struct tm modified_time;
    if (gmtime_r(&modified_seconds, &modified_time) == NULL) {
        snprintf(text, text_size, "%s", size);
        return;
    }
    /* Moonraker supplies an absolute Unix timestamp.  The pendant does not
     * maintain a timezone setting, therefore make the UTC display explicit. */
    snprintf(text, text_size, "%s | %04d-%02d-%02d %02d:%02d UTC", size,
             modified_time.tm_year + 1900, modified_time.tm_mon + 1,
             modified_time.tm_mday, modified_time.tm_hour, modified_time.tm_min);
}

static void format_print_time(char *text, size_t text_size, uint32_t seconds)
{
    if (seconds == 0) strlcpy(text, "Time unavailable", text_size);
    else snprintf(text, text_size, "%luh %02lum", (unsigned long)(seconds / 3600U),
                  (unsigned long)((seconds % 3600U) / 60U));
}

static void format_total_print_time(char *text, size_t text_size, uint64_t seconds)
{
    const uint64_t days = seconds / 86400U;
    const uint64_t hours = (seconds % 86400U) / 3600U;
    const uint64_t minutes = (seconds % 3600U) / 60U;
    if (days > 0) snprintf(text, text_size, "%llud %lluh %02llum",
                           (unsigned long long)days, (unsigned long long)hours,
                           (unsigned long long)minutes);
    else snprintf(text, text_size, "%lluh %02llum", (unsigned long long)hours,
                  (unsigned long long)minutes);
}

static void format_storage_size(char *text, size_t text_size, uint64_t bytes)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) snprintf(text, text_size, "%llu %s", (unsigned long long)bytes, units[unit]);
    else snprintf(text, text_size, "%.1f %s", value, units[unit]);
}

static void format_timestamp(char *text, size_t text_size, uint64_t milliseconds)
{
    if (milliseconds == 0) {
        strlcpy(text, "Unavailable", text_size);
        return;
    }
    time_t seconds = (time_t)(milliseconds / 1000U);
    struct tm value;
    if (gmtime_r(&seconds, &value) == NULL) {
        strlcpy(text, "Unavailable", text_size);
        return;
    }
    snprintf(text, text_size, "%02d.%02d.%02d %02d:%02d", value.tm_mday,
             value.tm_mon + 1, (value.tm_year + 1900) % 100,
             value.tm_hour, value.tm_min);
}

static void format_history_list_time(char *text, size_t text_size, uint64_t milliseconds)
{
    if (milliseconds == 0) {
        strlcpy(text, "Unknown time", text_size);
        return;
    }
    time_t seconds = (time_t)(milliseconds / 1000U);
    struct tm value;
    if (gmtime_r(&seconds, &value) == NULL) {
        strlcpy(text, "Unknown time", text_size);
        return;
    }
    snprintf(text, text_size, "%02d.%02d.%02d %02d:%02d", value.tm_mday,
             value.tm_mon + 1, (value.tm_year + 1900) % 100,
             value.tm_hour, value.tm_min);
}

static void open_parent_directory(void)
{
    moonraker_get_file_browser(&file_browser_snapshot);
    if (strcmp(file_browser_snapshot.path, "gcodes") == 0) return;
    char parent[sizeof(file_browser_snapshot.path)];
    strlcpy(parent, file_browser_snapshot.path, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash == NULL || strcmp(parent, "gcodes") == 0) return;
    *slash = '\0';
    files_page_offset = 0;
    moonraker_request_directory(parent[0] ? parent : "gcodes");
    navigate_to(PAGE_FILES);
}

static void brightness_event(lv_event_t *event)
{
    uint8_t value = board_get_backlight();
    value = value <= 30 ? 100 : value - 10;
    if (board_set_backlight(value) == ESP_OK) {
        char text[12];
        snprintf(text, sizeof(text), "%u%%", value);
        ui_kit_set_row_text(lv_event_get_target(event), "Display", text);
    }
}

static lv_obj_t *create_monitor_panel(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

static void monitor_status_pulse_cb(void *object, int32_t opacity)
{
    lv_obj_set_style_text_opa(object, (lv_opa_t)opacity, LV_PART_MAIN);
}

static void start_monitor_status_pulse(const printer_state_t *printer)
{
    const bool should_pulse = printer->connection == PRINTER_CONNECTION_ONLINE &&
                              !printer_is_error(printer);
    if (monitor_state_label == NULL || should_pulse == monitor_status_pulsing) return;

    if (!should_pulse) {
        lv_anim_del(monitor_state_label, NULL);
        lv_obj_set_style_text_opa(monitor_state_label, LV_OPA_COVER, LV_PART_MAIN);
        monitor_status_pulsing = false;
        return;
    }

    /* A slow brightness pulse makes a live job recognizable at a glance
     * without competing with the progress bar or filename. */
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, monitor_state_label);
    lv_anim_set_exec_cb(&animation, monitor_status_pulse_cb);
    lv_anim_set_values(&animation, LV_OPA_COVER, LV_OPA_60);
    lv_anim_set_time(&animation, 900);
    lv_anim_set_playback_time(&animation, 900);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
    monitor_status_pulsing = true;
}

static void add_monitoring_card(const printer_state_t *printer)
{
    /* The monitor's four blocks plus its final action row must fit in the
     * 348 px viewport.  At 154 px the Start print row made the page overflow
     * by a few pixels and LVGL began scrolling it as soon as a job completed. */
    lv_obj_t *card = create_monitor_panel(page_body, LV_PCT(100), 148);
    lv_obj_set_style_bg_color(card, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);

    monitor_state_label = lv_label_create(card);
    lv_label_set_text(monitor_state_label, printer_is_error(printer) ? "ERROR" :
                             printer->job_state == PRINTER_JOB_PAUSED ? "PAUSED" :
                             printer->job_state == PRINTER_JOB_PRINTING ? "PRINTING" :
                             printer->operational_state == PRINTER_STATE_READY ? "READY" : "OFFLINE");
    lv_obj_set_style_text_font(monitor_state_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(monitor_state_label, printer_is_error(printer) ? ui_kit_tone_color(UI_TONE_DANGER) :
                                 printer_is_printing(printer) ? ui_kit_tone_color(UI_TONE_SUCCESS) :
                                 ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_align(monitor_state_label, LV_ALIGN_TOP_LEFT, 0, 0);
    start_monitor_status_pulse(printer);

    monitor_job_label = lv_label_create(card);
    lv_label_set_text(monitor_job_label, printer->filename[0] ? printer->filename : "No active print");
    /* The active filename stays readable in place. Reserve two lines instead
     * of making the Monitor page itself scrollable. */
    lv_label_set_long_mode(monitor_job_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(monitor_job_label, LV_PCT(100),
                    lv_font_get_line_height(LV_FONT_DEFAULT) * 2);
    lv_obj_set_style_pad_all(monitor_job_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(monitor_job_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
    lv_obj_align(monitor_job_label, LV_ALIGN_TOP_LEFT, 0, 25);

    char total[32];
    char eta[32];
    format_monitor_print_time(total, sizeof(total), eta, sizeof(eta), printer,
                              monitor_estimated_time_seconds);
    monitor_total_time_label = lv_label_create(card);
    lv_label_set_text(monitor_total_time_label, total);
    lv_obj_set_style_text_color(monitor_total_time_label, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_align(monitor_total_time_label, LV_ALIGN_TOP_LEFT, 0, 70);
    monitor_eta_label = lv_label_create(card);
    lv_label_set_text(monitor_eta_label, eta);
    lv_obj_set_style_text_color(monitor_eta_label, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_align(monitor_eta_label, LV_ALIGN_TOP_RIGHT, 0, 70);

    monitor_progress_label = lv_label_create(card);
    lv_label_set_text_fmt(monitor_progress_label, "%u.%u%%", printer->progress_tenths_percent / 10,
                          printer->progress_tenths_percent % 10);
    lv_obj_set_style_text_font(monitor_progress_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(monitor_progress_label, ui_kit_tone_color(UI_TONE_ACCENT), LV_PART_MAIN);
    lv_obj_align(monitor_progress_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    monitor_progress_bar = lv_bar_create(card);
    lv_obj_set_size(monitor_progress_bar, 164, 8);
    lv_obj_align(monitor_progress_bar, LV_ALIGN_BOTTOM_RIGHT, 0, -5);
    lv_bar_set_range(monitor_progress_bar, 0, 100);
    lv_bar_set_value(monitor_progress_bar, printer->progress_tenths_percent / 10, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(monitor_progress_bar, ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(monitor_progress_bar, ui_kit_tone_color(UI_TONE_ACCENT), LV_PART_INDICATOR);
}

static void add_temperature_and_position(const printer_state_t *printer)
{
    lv_obj_t *temperatures = lv_obj_create(page_body);
    lv_obj_set_size(temperatures, LV_PCT(100), 70);
    lv_obj_set_flex_flow(temperatures, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(temperatures, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(temperatures, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(temperatures, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(temperatures, 7, LV_PART_MAIN);
    lv_obj_clear_flag(temperatures, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *hotend = create_monitor_panel(temperatures, 146, 70);
    lv_obj_t *bed = create_monitor_panel(temperatures, 146, 70);
    lv_obj_t *label = lv_label_create(hotend);
    lv_label_set_text(label, "HOTEND");
    lv_obj_set_style_text_color(label, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    monitor_hotend_label = lv_label_create(hotend);
    lv_label_set_text_fmt(monitor_hotend_label, "%d / %d °C", printer->hotend_current_deci_c / 10,
                          printer->hotend_target_deci_c / 10);
    lv_obj_set_style_text_font(monitor_hotend_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(monitor_hotend_label, ui_kit_tone_color(UI_TONE_WARNING), LV_PART_MAIN);
    lv_obj_align(monitor_hotend_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    label = lv_label_create(bed);
    lv_label_set_text(label, "BED");
    lv_obj_set_style_text_color(label, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    monitor_bed_label = lv_label_create(bed);
    lv_label_set_text_fmt(monitor_bed_label, "%d / %d °C", printer->bed_current_deci_c / 10,
                          printer->bed_target_deci_c / 10);
    lv_obj_set_style_text_font(monitor_bed_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(monitor_bed_label, ui_kit_tone_color(UI_TONE_WARNING), LV_PART_MAIN);
    lv_obj_align(monitor_bed_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Keep a separate baseline for the title and coordinates.  At 52 px the
     * two text rows overlap with the display's actual font metrics. */
    lv_obj_t *position = create_monitor_panel(page_body, LV_PCT(100), 60);
    label = lv_label_create(position);
    lv_label_set_text(label, "TOOLHEAD POSITION");
    lv_obj_set_style_text_color(label, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    monitor_position_label = lv_label_create(position);
    char text[96];
    format_position(text, sizeof(text), printer);
    lv_label_set_text(monitor_position_label, text);
    lv_label_set_recolor(monitor_position_label, true);
    lv_obj_set_style_text_color(monitor_position_label, ui_kit_tone_color(UI_TONE_ACCENT), LV_PART_MAIN);
    lv_obj_align(monitor_position_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void set_bottom_navigation(page_t page)
{
    static const page_t pages[] = { PAGE_STATUS, PAGE_TUNE, PAGE_MOVE, PAGE_DATA, PAGE_SERVICE };
    printer_state_t printer;
    printer_get_state(&printer);
    const bool printer_online = printer.connection == PRINTER_CONNECTION_ONLINE;
    const bool printer_page = is_printer_page(page);
    const bool monitoring_page = page == PAGE_STATUS;
    const bool move_page = page == PAGE_MOVE;
    const bool files_pagination_visible = page == PAGE_FILES &&
                                          !file_browser_snapshot.loading &&
                                          file_browser_snapshot.valid &&
                                          file_browser_snapshot.entry_count > MOONRAKER_FILE_BROWSER_PAGE_SIZE;
    const bool history_pagination_visible = page == PAGE_HISTORY &&
                                            !history_snapshot.loading && history_snapshot.valid &&
                                            history_snapshot.count > 0;
    const bool pagination_visible = files_pagination_visible || history_pagination_visible;
    lv_obj_set_height(page_body, printer_page ? (monitoring_page || move_page ? 348 :
                                                   pagination_visible ? 286 : 324) : 414);
    lv_obj_align(page_body, LV_ALIGN_TOP_MID, 0,
                 printer_page ? (monitoring_page || move_page ? 60 :
                                 pagination_visible ? 122 : 84) : 58);
    lv_obj_align(page_title, LV_ALIGN_TOP_LEFT, 12, printer_page ? 57 : 16);
    lv_obj_align(back_hint, LV_ALIGN_BOTTOM_LEFT, 12, printer_page ? -68 : -14);
    if (printer_page) {
        lv_obj_clear_flag(header_printer_button, LV_OBJ_FLAG_HIDDEN);
        if (command_feedback_label != NULL) {
            if (command_feedback_visible) lv_obj_clear_flag(command_feedback_label, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(command_feedback_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(header_printer_button, LV_OBJ_FLAG_HIDDEN);
        if (command_feedback_label != NULL) lv_obj_add_flag(command_feedback_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (page != PAGE_FILES && page != PAGE_HISTORY && files_pagination != NULL) {
        lv_obj_add_flag(files_pagination, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (!printer_page) {
            lv_obj_add_flag(bottom_navigation[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(bottom_navigation[i], LV_OBJ_FLAG_HIDDEN);
        if (i == 0 || printer_online) {
            lv_obj_clear_state(bottom_navigation[i], LV_STATE_DISABLED);
            lv_group_add_obj(navigation_group, bottom_navigation[i]);
        } else {
            lv_obj_add_state(bottom_navigation[i], LV_STATE_DISABLED);
            lv_group_remove_obj(bottom_navigation[i]);
        }
        lv_obj_set_style_bg_color(bottom_navigation[i],
            (page == pages[i] || (pages[i] == PAGE_STATUS && page == PAGE_START_PRINT) ||
             (pages[i] == PAGE_MOVE && page == PAGE_MACROS) ||
             (pages[i] == PAGE_DATA && (page == PAGE_FILES || page == PAGE_FILE_DETAILS ||
                                        page == PAGE_HISTORY || page == PAGE_HISTORY_DETAILS)) ||
             (pages[i] == PAGE_SERVICE && (page == PAGE_CONSOLE || page == PAGE_SYSTEM_INFO ||
                                           page == PAGE_PROBE_CALIBRATE || page == PAGE_PROMPT))) ?
                ui_kit_color(UI_COLOR_SURFACE_FOCUSED) : ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_text_color(bottom_navigation[i],
            (page == pages[i] || (pages[i] == PAGE_STATUS && page == PAGE_START_PRINT) ||
             (pages[i] == PAGE_MOVE && page == PAGE_MACROS) ||
             (pages[i] == PAGE_DATA && (page == PAGE_FILES || page == PAGE_FILE_DETAILS ||
                                        page == PAGE_HISTORY || page == PAGE_HISTORY_DETAILS)) ||
             (pages[i] == PAGE_SERVICE && (page == PAGE_CONSOLE || page == PAGE_SYSTEM_INFO ||
                                           page == PAGE_PROBE_CALIBRATE || page == PAGE_PROMPT))) ?
                ui_kit_tone_color(UI_TONE_ACCENT) : ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    }
}

static void navigate_to(page_t page)
{
    page_t previous_page = current_page;
    if (previous_page == PAGE_FIRMWARE && page != PAGE_FIRMWARE) {
        ota_update_release_catalog();
        moonraker_set_suspended(false);
    }
    if (previous_page != PAGE_FIRMWARE && page == PAGE_FIRMWARE) {
        /* The WebSocket's buffers and polling compete directly with GitHub
         * TLS.  Firmware owns those resources until the user leaves it. */
        moonraker_set_suspended(true);
    }
    if (previous_page == PAGE_CONSOLE && page != PAGE_CONSOLE) console_buffers_release();
    if ((previous_page == PAGE_HISTORY || previous_page == PAGE_HISTORY_DETAILS ||
         previous_page == PAGE_START_PRINT) &&
        page != PAGE_HISTORY && page != PAGE_HISTORY_DETAILS && page != PAGE_START_PRINT) {
        moonraker_release_history();
    }
    if ((previous_page == PAGE_EXCLUDE_OBJECTS || previous_page == PAGE_EXCLUDE_CONFIRM) &&
        page != PAGE_EXCLUDE_OBJECTS && page != PAGE_EXCLUDE_CONFIRM) {
        moonraker_release_exclude_objects();
        heap_caps_free(exclude_objects_snapshot);
        exclude_objects_snapshot = NULL;
        heap_caps_free(exclude_map_points);
        exclude_map_points = NULL;
        exclude_objects_generation_seen = 0;
    }
    if (previous_page == PAGE_DIAGNOSTICS) diagnostics_label = NULL;
    if (previous_page == PAGE_BATTERY) battery_debug_label = NULL;
    if (page != PAGE_MOVE) move_mode = MOVE_MODE_BROWSE;
    if (page != PAGE_TUNE) tune_mode = TUNE_MODE_BROWSE;
    current_page = page;
    clear_page();
    switch (page) {
    case PAGE_FLEET:
    {
        lv_label_set_text(page_title, "Printers");
        lv_label_set_text(back_hint, "Select a printer or open Settings");
        size_t count = 0;
        settings_get_printers(printer_list_snapshot, SETTINGS_MAX_PRINTERS, &count);
        size_t active_index = SIZE_MAX;
        settings_get_active_printer_index(&active_index);
        if (count == 0) {
            add_nav_card("No printers configured", "Open Settings to add one", "SETUP",
                         UI_TONE_WARNING, navigation_event, (void *)PAGE_SETTINGS);
        }
        for (size_t i = 0; i < count; ++i) {
            const char *state = i == active_index ? "ACTIVE" : "SELECT";
            const char *subtitle = printer_list_snapshot[i].host[0] ? printer_list_snapshot[i].host : "Moonraker not configured";
            fleet_printer_button = add_nav_card(printer_list_snapshot[i].name, subtitle, state,
                                                i == active_index ? UI_TONE_SUCCESS : UI_TONE_DEFAULT,
                                                select_printer_event, (void *)(uintptr_t)i);
        }
        /* Fleet owns static configuration cards; live status belongs to the
         * selected printer screen and must not overwrite the last card here. */
        fleet_printer_button = NULL;
        ui_kit_create_section(page_body, "PENDANT");
        add_nav_row("Settings", "", UI_TONE_DEFAULT, true,
                    navigation_event, (void *)PAGE_SETTINGS);
        break;
    }
    case PAGE_STATUS:
    {
        printer_state_t printer;
        printer_get_state(&printer);
        lv_label_set_text(page_title, "");
        lv_label_set_text(back_hint, "");
        add_monitoring_card(&printer);
        add_temperature_and_position(&printer);
        if (printer.job_state == PRINTER_JOB_PAUSED || printer.job_state == PRINTER_JOB_PRINTING) {
            add_print_actions(&printer);
        } else if (print_start_available(&printer)) {
            add_nav_row("Start print", "", UI_TONE_SUCCESS, true,
                        navigation_event, (void *)PAGE_START_PRINT);
        }
        break;
    }
    case PAGE_TUNE:
    {
        printer_state_t printer;
        printer_get_state(&printer);
        printer_get_capabilities(&tune_capabilities);
        if (printer.tune_values_valid) {
            tune_offset_centi_mm = printer.z_offset_centi_mm;
            tune_speed_percent = printer.speed_percent;
            tune_flow_percent = printer.flow_percent;
        }
        const bool available = printer.connection == PRINTER_CONNECTION_ONLINE;
        char value[28];
        lv_label_set_text(page_title, "Tune");
        lv_label_set_text(back_hint, "Hold = apply  |  Back = cancel");
        ui_kit_create_section(page_body, "MOTION");
        tune_offset_row = add_nav_row("Z Offset", "+0.00 mm", UI_TONE_ACCENT, available,
                                      tune_value_event, (void *)(uintptr_t)(TUNE_MODE_OFFSET << 8));
        char offset[28];
        const char offset_sign = tune_offset_centi_mm < 0 ? '-' : '+';
        const uint32_t offset_magnitude = (uint32_t)(tune_offset_centi_mm < 0 ?
            -tune_offset_centi_mm : tune_offset_centi_mm);
        snprintf(offset, sizeof(offset), "%c%lu.%02lu mm", offset_sign,
                 (unsigned long)(offset_magnitude / 100U), (unsigned long)(offset_magnitude % 100U));
        ui_kit_set_row_text(tune_offset_row, "Z Offset", offset);
        if (tune_capabilities.speed_factor) {
            snprintf(value, sizeof(value), "%ld%%", (long)tune_speed_percent);
            tune_speed_row = add_nav_row("Speed", value, UI_TONE_ACCENT, available,
                                         tune_value_event, (void *)(uintptr_t)(TUNE_MODE_SPEED << 8));
        }
        if (tune_capabilities.flow_factor) {
            snprintf(value, sizeof(value), "%ld%%", (long)tune_flow_percent);
            tune_flow_row = add_nav_row("Flow", value, UI_TONE_ACCENT, available,
                                        tune_value_event, (void *)(uintptr_t)(TUNE_MODE_FLOW << 8));
        }
        if (tune_capabilities.fan_count > 0) ui_kit_create_section(page_body, "FANS");
        for (size_t i = 0; i < tune_capabilities.fan_count; ++i) {
            const printer_fan_capability_t *fan = &tune_capabilities.fans[i];
            snprintf(value, sizeof(value), "%u%%", fan->speed_percent);
            tune_fan_rows[i] = add_nav_row(fan->label, value, UI_TONE_ACCENT,
                                           fan->available && fan->controllable,
                                           tune_value_event,
                                           (void *)(uintptr_t)((TUNE_MODE_FAN << 8) | i));
        }
        if (tune_capabilities.heater_count > 0) ui_kit_create_section(page_body, "HEATERS");
        for (size_t i = 0; i < tune_capabilities.heater_count; ++i) {
            const printer_heater_capability_t *heater = &tune_capabilities.heaters[i];
            snprintf(value, sizeof(value), "%d.%d / %d.%d C",
                     heater->current_deci_c / 10, abs(heater->current_deci_c % 10),
                     heater->target_deci_c / 10, abs(heater->target_deci_c % 10));
            tune_heater_rows[i] = add_nav_row(heater->label, value, UI_TONE_ACCENT,
                                              heater->available && heater->controllable,
                                              tune_value_event,
                                              (void *)(uintptr_t)((TUNE_MODE_HEATER << 8) | i));
        }
        if (printer.job_state == PRINTER_JOB_PRINTING || printer.job_state == PRINTER_JOB_PAUSED) {
            ui_kit_create_section(page_body, "PRINT");
            add_nav_row("Exclude objects", "OPEN", UI_TONE_WARNING, true, exclude_objects_event, NULL);
        }
        break;
    }
    case PAGE_EXCLUDE_OBJECTS:
    {
        if (exclude_objects_snapshot == NULL) { navigate_to(PAGE_TUNE); break; }
        moonraker_get_exclude_objects(exclude_objects_snapshot);
        exclude_objects_generation_seen = exclude_objects_snapshot->generation;
        lv_label_set_text(page_title, "Exclude objects");
        lv_label_set_text(back_hint, "Tap a part, then hold to exclude");
        if (exclude_objects_snapshot->loading) {
            add_nav_row("Loading object markers...", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else if (!exclude_objects_snapshot->valid || exclude_objects_snapshot->count == 0) {
            add_nav_row(exclude_objects_snapshot->error[0] ? exclude_objects_snapshot->error :
                        "No object markers in this file", NULL,
                        exclude_objects_snapshot->error[0] ? UI_TONE_DANGER : UI_TONE_MUTED,
                        false, NULL, NULL);
        } else {
            add_exclude_map();
        }
        break;
    }
    case PAGE_EXCLUDE_CONFIRM:
    {
        lv_label_set_text(page_title, "Exclude object?");
        lv_label_set_text(back_hint, "Back = keep printing");
        if (exclude_objects_snapshot != NULL && exclude_selected_index < exclude_objects_snapshot->count) {
            const moonraker_exclude_object_t *object = &exclude_objects_snapshot->objects[exclude_selected_index];
            add_exclude_confirmation(object);
        }
        break;
    }
    case PAGE_MOVE:
    {
        printer_state_t printer;
        printer_get_state(&printer);
        const bool available = move_commands_available();
        lv_label_set_text(page_title, "");
        lv_label_set_text(back_hint, "Hold = move/apply  |  Back = Monitor");
        add_nav_hold_row("E-STOP", "HOLD", UI_TONE_DANGER, emergency_stop_event, NULL);
        move_position_section = ui_kit_create_section(page_body, "POSITION (mm)");
        for (uint8_t axis = 0; axis < 3; ++axis) {
            char title[32];
            format_axis_title(title, sizeof(title), &printer, axis);
            move_axis_rows[axis] = add_nav_row(title, " ", UI_TONE_ACCENT, available,
                                                move_axis_event, (void *)(uintptr_t)axis);
            if (move_axis_rows[axis] != NULL) {
                lv_obj_t *title_label = lv_obj_get_child(move_axis_rows[axis], 0);
                if (title_label != NULL) lv_label_set_recolor(title_label, true);
            }
        }
        move_jog_section = ui_kit_create_section(page_body, "JOG");
        char step[24];
        const int32_t selected_step = move_steps_centi_mm[move_step_index];
        snprintf(step, sizeof(step), "%ld.%02ld mm", (long)(selected_step / 100),
                 (long)(selected_step % 100));
        move_step_row = add_nav_row("Step", step, UI_TONE_ACCENT, available, move_step_event, NULL);
        move_park_row = add_nav_row("Park", "ALL", UI_TONE_WARNING, available, move_park_event, NULL);
        move_extrusion_section = ui_kit_create_section(page_body, "EXTRUSION");
        move_extruder_feed_row = add_nav_row("Extruder feed", "", UI_TONE_ACCENT, available,
                                             move_extruder_feed_event, NULL);
        char extruder_speed[24];
        snprintf(extruder_speed, sizeof(extruder_speed), "%ld mm/s",
                 (long)move_extruder_speed_mm_s);
        move_extruder_speed_row = add_nav_row("Extruder speed", extruder_speed, UI_TONE_ACCENT,
                                              available, move_extruder_speed_event, NULL);
        add_nav_card("Macros", "User commands", ">", UI_TONE_DEFAULT,
                     navigation_event, (void *)PAGE_MACROS);
        break;
    }
    case PAGE_MACROS:
        moonraker_get_macros(&macros_snapshot);
        /* Macro definitions can change outside the pendant (for example,
         * after editing printer.cfg).  Refresh on entry instead of treating
         * a previously valid snapshot as permanent.  Rebuilds caused by the
         * request's generation changes keep previous_page == PAGE_MACROS,
         * preventing a polling loop. */
        if (previous_page != PAGE_MACROS && !macros_snapshot.loading) {
            (void)moonraker_request_macros();
            moonraker_get_macros(&macros_snapshot);
        }
        macros_generation_seen = macros_snapshot.generation;
        lv_label_set_text(page_title, "Macros");
        lv_label_set_text(back_hint, "Hold a macro to run it  |  Back: Move");
        /* A refresh retains a valid snapshot, so only show a loader before
         * the first result.  Later responses rebuild this list in place. */
        if (macros_snapshot.loading && !macros_snapshot.valid) {
            add_nav_row("Loading macros...", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else if (macros_snapshot.error[0] != '\0' && !macros_snapshot.valid) {
            add_nav_row(macros_snapshot.error, NULL, UI_TONE_DANGER, false, NULL, NULL);
        } else {
            size_t visible_count = 0;
            for (size_t i = 0; i < macros_snapshot.count; ++i) {
                if (macros_snapshot.names[i][0] == '_') continue;
                add_nav_wrapped_hold_card(macros_snapshot.names[i], UI_TONE_DEFAULT,
                                          macro_event, (void *)(uintptr_t)i);
                ++visible_count;
            }
            if (macros_snapshot.valid && visible_count == 0) {
                add_nav_row("No user macros configured", NULL, UI_TONE_MUTED, false, NULL, NULL);
            }
        }
        break;
    case PAGE_DATA:
    {
        moonraker_get_history_totals(&history_totals_snapshot);
        if (!history_totals_snapshot.valid && !history_totals_snapshot.loading) {
            (void)moonraker_request_history_totals();
            moonraker_get_history_totals(&history_totals_snapshot);
        }
        history_totals_generation_seen = history_totals_snapshot.generation;
        lv_label_set_text(page_title, "Data");
        lv_label_set_text(back_hint, "Back: Monitor");
        add_nav_card("Files", "Browse G-code files", ">", UI_TONE_DEFAULT,
                     navigation_event, (void *)PAGE_FILES);
        add_nav_card("History", "Completed and failed jobs", ">", UI_TONE_DEFAULT,
                     navigation_event, (void *)PAGE_HISTORY);
        ui_kit_create_section(page_body, "PRINT STATISTICS");
        if (history_totals_snapshot.loading) {
            add_nav_row("Loading statistics...", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else if (history_totals_snapshot.error[0] != '\0') {
            add_nav_row(history_totals_snapshot.error, NULL, UI_TONE_WARNING, false, NULL, NULL);
        } else if (history_totals_snapshot.valid) {
            char value[32];
            snprintf(value, sizeof(value), "%llu", (unsigned long long)history_totals_snapshot.total_jobs);
            add_nav_row("Total print jobs", value, UI_TONE_DEFAULT, false, NULL, NULL);
            format_total_print_time(value, sizeof(value), history_totals_snapshot.total_time_seconds);
            add_nav_row("Total time", value, UI_TONE_DEFAULT, false, NULL, NULL);
            format_total_print_time(value, sizeof(value), history_totals_snapshot.total_print_time_seconds);
            add_nav_row("Total print time", value, UI_TONE_DEFAULT, false, NULL, NULL);
            snprintf(value, sizeof(value), "%llu.%03llum",
                     (unsigned long long)(history_totals_snapshot.total_filament_used_mm / 1000U),
                     (unsigned long long)(history_totals_snapshot.total_filament_used_mm % 1000U));
            add_nav_row("Total filament", value, UI_TONE_DEFAULT, false, NULL, NULL);
            format_total_print_time(value, sizeof(value), history_totals_snapshot.longest_print_seconds);
            add_nav_row("Longest print", value, UI_TONE_DEFAULT, false, NULL, NULL);
        }
        break;
    }
    case PAGE_HISTORY:
        moonraker_get_history_status(&history_snapshot);
        if (!history_snapshot.valid && !history_snapshot.loading && !history_snapshot.unavailable) {
            (void)moonraker_request_history(history_snapshot.offset);
            moonraker_get_history_status(&history_snapshot);
        }
        history_generation_seen = history_snapshot.generation;
        lv_label_set_text(page_title, "History");
        lv_label_set_text(back_hint, "Back: Data");
        if (history_snapshot.loading) {
            add_nav_row("Loading history...", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else if (history_snapshot.unavailable) {
            add_nav_row("History unavailable", NULL, UI_TONE_WARNING, false, NULL, NULL);
        } else if (history_snapshot.error[0] != '\0') {
            add_nav_row(history_snapshot.error, NULL, UI_TONE_DANGER, false, NULL, NULL);
        } else if (history_snapshot.valid && history_snapshot.count == 0) {
            add_nav_row("No print history", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else {
            for (size_t i = 0; i < history_snapshot.count; ++i) {
                moonraker_history_job_t job;
                if (!moonraker_get_history_job(i, &job)) continue;
                char detail[80];
                char finished[24];
                char duration[24];
                format_history_list_time(finished, sizeof(finished), job.end_time_ms);
                format_print_time(duration, sizeof(duration), job.duration_seconds);
                snprintf(detail, sizeof(detail), "%s | %s | %s",
                         job.status[0] ? job.status : "UNKNOWN", finished, duration);
                add_nav_wrapped_card(job.filename[0] ? job.filename : "Unknown file", detail, ">",
                                     UI_TONE_DEFAULT, history_entry_event, (void *)(uintptr_t)i);
            }
        }
        update_history_pagination();
        break;
    case PAGE_HISTORY_DETAILS:
    {
        char detail[80];
        lv_label_set_text(page_title, "Print job");
        lv_label_set_text(back_hint, "Back: History");
        if (selected_history_job.file_exists && selected_history_job.filename[0]) {
            strlcpy(selected_file_path, selected_history_job.filename, sizeof(selected_file_path));
            printer_state_t printer;
            printer_get_state(&printer);
            if (print_start_available(&printer)) {
                add_nav_hold_row("Repeat print", "HOLD", UI_TONE_SUCCESS,
                                 start_selected_file_event, NULL);
            }
        }
        ui_kit_create_section(page_body, selected_history_job.filename[0] ? selected_history_job.filename : "Unknown file");
        add_nav_row("Result", selected_history_job.status[0] ? selected_history_job.status : "Unavailable", UI_TONE_DEFAULT, false, NULL, NULL);
        if (selected_history_job.start_time_ms) {
            format_timestamp(detail, sizeof(detail), selected_history_job.start_time_ms);
            add_nav_row("Started UTC", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        }
        if (selected_history_job.end_time_ms) {
            format_timestamp(detail, sizeof(detail), selected_history_job.end_time_ms);
            add_nav_row("Finished UTC", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        }
        format_print_time(detail, sizeof(detail), selected_history_job.duration_seconds);
        add_nav_row("Duration", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        if (selected_history_job.filament_used_mm > 0) {
            snprintf(detail, sizeof(detail), "%lu.%lum", (unsigned long)(selected_history_job.filament_used_mm / 1000U),
                     (unsigned long)(selected_history_job.filament_used_mm % 1000U));
            add_nav_row("Filament", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        }
        if (selected_history_job.message[0]) add_nav_wrapped_card("Message", selected_history_job.message, "", UI_TONE_WARNING, NULL, NULL);
        break;
    }
    case PAGE_FILES:
    {
        moonraker_get_file_browser(&file_browser_snapshot);
        if (previous_page != PAGE_FILES && previous_page != PAGE_FILE_DETAILS &&
            !file_browser_snapshot.loading) {
            const char *refresh_path = file_browser_snapshot.path[0] ? file_browser_snapshot.path : "gcodes";
            if (moonraker_request_directory(refresh_path) == ESP_OK) file_browser_snapshot.loading = true;
        }
        const char *relative_path = file_browser_snapshot.path;
        if (strncmp(relative_path, "gcodes/", strlen("gcodes/")) == 0) relative_path += strlen("gcodes/");
        if (*relative_path == '\0' || strcmp(relative_path, "gcodes") == 0) {
            lv_label_set_text(page_title, "Files");
        } else {
            lv_label_set_text_fmt(page_title, "Files / %s", relative_path);
        }
        lv_label_set_text(back_hint, strcmp(file_browser_snapshot.path, "gcodes") == 0 ? "Select a file or folder" : "Back: parent folder");
        if (!file_browser_snapshot.loading &&
            (!file_browser_snapshot.valid || file_browser_snapshot.path[0] == '\0')) {
            /* The request updates Moonraker's shared state, not this local
             * snapshot.  Reflect it here as well so the first visit renders
             * a loading row instead of an empty page. */
            if (moonraker_request_directory("gcodes") == ESP_OK) {
                file_browser_snapshot.loading = true;
            }
        }
        if (file_browser_snapshot.valid && file_browser_snapshot.entry_count > 0 &&
            files_page_offset >= file_browser_snapshot.entry_count) {
            files_page_offset = 0;
        }
        update_files_pagination();
        if (file_browser_snapshot.loading) {
            add_nav_row("Loading files...", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else if (file_browser_snapshot.error[0] != '\0') {
            add_nav_row(file_browser_snapshot.error, NULL, UI_TONE_DANGER, false, NULL, NULL);
        } else if (file_browser_snapshot.valid && file_browser_snapshot.entry_count == 0) {
            add_nav_row("Folder is empty", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else {
            char detail[40];
            moonraker_get_file_browser_page(files_page_offset, &file_browser_page_snapshot);
            for (size_t i = 0; i < file_browser_page_snapshot.entry_count; ++i) {
                const moonraker_file_entry_t *entry = &file_browser_page_snapshot.entries[i];
                const size_t index = file_browser_page_snapshot.offset + i;
                if (entry->is_directory) {
                    add_nav_wrapped_card(entry->name, "Folder", ">", UI_TONE_ACCENT, file_entry_event, (void *)(uintptr_t)index);
                } else {
                    format_file_details(detail, sizeof(detail), entry->size_bytes,
                                        entry->modified_ms);
                    add_nav_wrapped_card(entry->name, detail, ">", UI_TONE_DEFAULT, file_entry_event, (void *)(uintptr_t)index);
                }
            }
        }
        files_generation_seen = file_browser_snapshot.generation;
        break;
    }
    case PAGE_FILE_DETAILS:
    {
        char detail[80];
        moonraker_file_metadata_t metadata;
        moonraker_get_file_metadata(&metadata);
        metadata_generation_seen = metadata.generation;
        if (metadata.valid) selected_file = metadata.file;
        lv_label_set_text(page_title, "Print file");
        lv_label_set_text(back_hint, "Back: Files");
        ui_kit_create_section(page_body, selected_file.name);
        if (metadata.loading) {
            add_nav_row("Metadata", "Loading...", UI_TONE_ACCENT, false, NULL, NULL);
        } else if (metadata.error[0] != '\0') {
            add_nav_row("Metadata", metadata.error, UI_TONE_WARNING, false, NULL, NULL);
        }
        format_print_time(detail, sizeof(detail), selected_file.estimated_time_seconds);
        add_nav_row("Estimated time", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        if (selected_file.filament_type[0]) strlcpy(detail, selected_file.filament_type, sizeof(detail));
        else if (selected_file.filament_name[0]) strlcpy(detail, selected_file.filament_name, sizeof(detail));
        else strlcpy(detail, "Unavailable", sizeof(detail));
        add_nav_row("Filament", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        if (selected_file.filament_weight_deci_g > 0) {
            snprintf(detail, sizeof(detail), "%u.%ug", selected_file.filament_weight_deci_g / 10,
                     selected_file.filament_weight_deci_g % 10);
        } else strlcpy(detail, "Unavailable", sizeof(detail));
        add_nav_row("Required", detail, UI_TONE_DEFAULT, false, NULL, NULL);
        printer_state_t printer;
        printer_get_state(&printer);
        if (metadata.valid && print_start_available(&printer)) {
            add_nav_hold_row("Start print", "HOLD", UI_TONE_SUCCESS, start_selected_file_event, NULL);
        } else if (!metadata.loading) {
            add_nav_row("Start print", "Printer not ready", UI_TONE_MUTED, false, NULL, NULL);
        }
        break;
    }
    case PAGE_SERVICE:
        lv_label_set_text(page_title, "Service");
        lv_label_set_text(back_hint, "Hold a control to run it");
        add_nav_card("Console", "G-code responses and commands", ">", UI_TONE_DEFAULT,
                     navigation_event, (void *)PAGE_CONSOLE);
        add_nav_card("Info", "Disk space", ">", UI_TONE_DEFAULT,
                     navigation_event, (void *)PAGE_SYSTEM_INFO);
        add_nav_card("Probe calibrate", "Calibrate probe Z offset", ">", UI_TONE_ACCENT,
                     navigation_event, (void *)PAGE_PROBE_CALIBRATE);
        ui_kit_create_section(page_body, "SYSTEM");
        add_nav_hold_row("Firmware restart", "HOLD", UI_TONE_DANGER,
                         service_gcode_event, "FIRMWARE_RESTART");
        add_nav_hold_row("Klipper restart", "HOLD", UI_TONE_DANGER,
                         service_gcode_event, "RESTART");
        add_nav_hold_row("Moonraker restart", "HOLD", UI_TONE_DANGER,
                         service_action_event, (void *)SERVICE_ACTION_MOONRAKER_RESTART);
        add_nav_hold_row("Save config", "HOLD", UI_TONE_WARNING,
                         service_gcode_event, "SAVE_CONFIG");
        add_nav_hold_row("System restart", "HOLD", UI_TONE_DANGER,
                         service_action_event, (void *)SERVICE_ACTION_SYSTEM_RESTART);
        add_nav_hold_row("System shutdown", "HOLD", UI_TONE_DANGER,
                         service_action_event, (void *)SERVICE_ACTION_SYSTEM_SHUTDOWN);
        break;
    case PAGE_PROBE_CALIBRATE:
    {
        moonraker_get_manual_probe(&manual_probe_snapshot);
        manual_probe_generation_seen = manual_probe_snapshot.generation;
        lv_label_set_text(page_title, "Probe calibrate");
        lv_label_set_text(back_hint, manual_probe_snapshot.is_active ?
                          "Adjust Z, then accept or abort" : "Back: Service");
        if (!manual_probe_snapshot.is_active) {
            ui_kit_create_section(page_body, "PROBE Z OFFSET");
            add_nav_row("Ready to calibrate", "", UI_TONE_DEFAULT, false, NULL, NULL);
            add_nav_hold_row("Start", "HOLD", UI_TONE_SUCCESS,
                             probe_gcode_event, "PROBE_CALIBRATE");
            break;
        }
        ui_kit_create_section(page_body, "CURRENT POSITION");
        char value[32];
        if (manual_probe_snapshot.position_valid)
            snprintf(value, sizeof(value), "%.3f mm", manual_probe_snapshot.z_position);
        else strlcpy(value, "Moving...", sizeof(value));
        add_nav_row("Z", value, UI_TONE_ACCENT, false, NULL, NULL);
        if (manual_probe_snapshot.lower_valid || manual_probe_snapshot.upper_valid) {
            char range[48];
            snprintf(range, sizeof(range), "%s%.3f / %s%.3f",
                     manual_probe_snapshot.lower_valid ? "" : "? ", manual_probe_snapshot.z_position_lower,
                     manual_probe_snapshot.upper_valid ? "" : "? ", manual_probe_snapshot.z_position_upper);
            add_nav_row("Bounds", range, UI_TONE_MUTED, false, NULL, NULL);
        }
        ui_kit_create_section(page_body, "ADJUST Z");
        add_nav_row("Down 1.0 mm", "-1.0", UI_TONE_ACCENT, true, probe_gcode_event, "TESTZ Z=-1");
        add_nav_row("Down 0.1 mm", "-0.1", UI_TONE_ACCENT, true, probe_gcode_event, "TESTZ Z=-0.1");
        add_nav_row("Down 0.05 mm", "-0.05", UI_TONE_ACCENT, true, probe_gcode_event, "TESTZ Z=-0.05");
        add_nav_row("Up 0.05 mm", "+0.05", UI_TONE_ACCENT, true, probe_gcode_event, "TESTZ Z=0.05");
        add_nav_row("Up 0.1 mm", "+0.1", UI_TONE_ACCENT, true, probe_gcode_event, "TESTZ Z=0.1");
        add_nav_row("Up 1.0 mm", "+1.0", UI_TONE_ACCENT, true, probe_gcode_event, "TESTZ Z=1");
        ui_kit_create_section(page_body, "FINISH");
        add_nav_hold_row("Accept", "HOLD", UI_TONE_SUCCESS, probe_gcode_event, "ACCEPT");
        add_nav_hold_row("Abort", "HOLD", UI_TONE_DANGER, probe_gcode_event, "ABORT");
        break;
    }
    case PAGE_PROMPT:
        moonraker_get_prompt(&prompt_snapshot);
        prompt_generation_seen = prompt_snapshot.generation;
        lv_label_set_text(page_title, prompt_snapshot.title[0] ? prompt_snapshot.title : "Prompt");
        lv_label_set_text(back_hint, "Hold an action to run it  |  Back: dismiss prompt");
        if (prompt_snapshot.text[0])
            add_nav_wrapped_card(prompt_snapshot.text,
                                 prompt_snapshot.truncated ? "Message truncated" : "",
                                 "", prompt_snapshot.truncated ? UI_TONE_WARNING : UI_TONE_DEFAULT,
                                 NULL, NULL);
        ui_kit_create_section(page_body, "ACTIONS");
        for (size_t i = 0; i < prompt_snapshot.button_count; ++i) {
            moonraker_prompt_button_t *button = &prompt_snapshot.buttons[i];
            if (button->command[0] != '\0') {
                add_nav_hold_row(button->label, button->footer ? "" : "HOLD",
                                 prompt_style_tone(button->style), prompt_button_event, button);
            } else {
                add_nav_row(button->label, button->footer ? "" : ">",
                            prompt_style_tone(button->style), false, NULL, NULL);
            }
        }
        if (prompt_snapshot.button_count == 0)
            add_nav_row("No actions", NULL, UI_TONE_MUTED, false, NULL, NULL);
        break;
    case PAGE_SYSTEM_INFO:
    {
        lv_label_set_text(page_title, "System info");
        lv_label_set_text(back_hint, "Back: Service");
        moonraker_get_disk_info(&disk_info_snapshot);
        if (!disk_info_snapshot.valid && !disk_info_snapshot.loading) {
            (void)moonraker_request_disk_info();
            moonraker_get_disk_info(&disk_info_snapshot);
        }
        disk_info_generation_seen = disk_info_snapshot.generation;
        ui_kit_create_section(page_body, "G-CODE STORAGE");
        if (disk_info_snapshot.loading) {
            add_nav_row("Disk", "Loading...", UI_TONE_MUTED, false, NULL, NULL);
        } else if (disk_info_snapshot.error[0] != '\0') {
            add_nav_row("Disk", disk_info_snapshot.error, UI_TONE_WARNING, false, NULL, NULL);
        } else if (disk_info_snapshot.valid) {
            char value[32];
            format_storage_size(value, sizeof(value), disk_info_snapshot.free_bytes);
            add_nav_row("Free", value, UI_TONE_SUCCESS, false, NULL, NULL);
            format_storage_size(value, sizeof(value), disk_info_snapshot.used_bytes);
            add_nav_row("Used", value, UI_TONE_WARNING, false, NULL, NULL);
            format_storage_size(value, sizeof(value), disk_info_snapshot.total_bytes);
            add_nav_row("Total", value, UI_TONE_DEFAULT, false, NULL, NULL);
            const uint32_t used_percent = disk_info_snapshot.total_bytes == 0 ? 0 :
                (uint32_t)((disk_info_snapshot.used_bytes * 100U) / disk_info_snapshot.total_bytes);
            snprintf(value, sizeof(value), "%u%%", (unsigned)used_percent);
            add_nav_row("Usage", value, used_percent >= 90 ? UI_TONE_DANGER :
                        used_percent >= 75 ? UI_TONE_WARNING : UI_TONE_SUCCESS, false, NULL, NULL);
        }
        break;
    }
    case PAGE_CONSOLE:
    {
        lv_label_set_text(back_hint, "Swipe output  |  Back: Service");
        if (!console_buffers_acquire()) {
            add_nav_row("Console unavailable", "Not enough PSRAM", UI_TONE_DANGER, false, NULL, NULL);
            break;
        }
        if (previous_page != PAGE_CONSOLE) {
            console_live = true;
            console_pending_count = 0;
        }
        lv_label_set_text(page_title, console_live ? "Console  LIVE" : "Console  PAUSED");
        lv_obj_set_style_text_color(page_title, ui_kit_tone_color(console_live ? UI_TONE_SUCCESS : UI_TONE_WARNING),
                                    LV_PART_MAIN);
        moonraker_get_console(console_snapshot);
        console_generation_seen = console_snapshot->generation;
        if (!console_snapshot->loading && console_snapshot->count == 0 && console_snapshot->error[0] == '\0') {
            (void)moonraker_request_gcode_store();
            moonraker_get_console(console_snapshot);
            console_generation_seen = console_snapshot->generation;
        }
        console_output = lv_obj_create(page_body);
        lv_obj_set_size(console_output, LV_PCT(100), 214);
        lv_obj_set_style_bg_color(console_output, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_color(console_output, ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(console_output, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(console_output, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_all(console_output, 6, LV_PART_MAIN);
        lv_obj_set_scroll_dir(console_output, LV_DIR_VER);
        lv_obj_add_event_cb(console_output, console_output_event, LV_EVENT_GESTURE, NULL);
        console_output_label = lv_label_create(console_output);
        lv_obj_set_width(console_output_label, LV_PCT(100));
        lv_label_set_long_mode(console_output_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(console_output_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(console_output_label, ui_kit_color(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        console_new_rows = lv_btn_create(console_output);
        lv_obj_set_size(console_new_rows, 104, 26);
        lv_obj_add_flag(console_new_rows, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(console_new_rows, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
        lv_obj_set_style_bg_color(console_new_rows, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(console_new_rows, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(console_new_rows, console_new_rows_event, LV_EVENT_CLICKED, NULL);
        lv_obj_t *new_label = lv_label_create(console_new_rows);
        lv_label_set_text(new_label, "↓ new output");
        lv_obj_center(new_label);
        add_nav_row("Command", !console_command[0] ? "EDIT" :
                    (strchr(console_command, '\n') != NULL ? "Multi-line script" : console_command), UI_TONE_DEFAULT,
                    true, console_command_event, NULL);
        add_nav_hold_row("Send", "HOLD", UI_TONE_WARNING, console_send_event, NULL);
        refresh_console_output();
        break;
    }
    case PAGE_START_PRINT:
    {
        lv_label_set_text(page_title, "Start print");
        lv_label_set_text(back_hint, "Choose a file  |  Back: Monitor");
        moonraker_get_history_status(&history_snapshot);
        if ((!history_snapshot.valid || history_snapshot.offset != 0) &&
            !history_snapshot.loading && !history_snapshot.unavailable) {
            (void)moonraker_request_history(0);
            moonraker_get_history_status(&history_snapshot);
        }
        history_generation_seen = history_snapshot.generation;
        if (history_snapshot.loading) {
            add_nav_row("Checking last print...", NULL, UI_TONE_MUTED, false, NULL, NULL);
        } else if (history_snapshot.valid && history_snapshot.count > 0 &&
                   moonraker_get_history_job(0, &last_print_job) &&
                   last_print_job.file_exists && last_print_job.filename[0] != '\0') {
            char detail[MOONRAKER_HISTORY_FILENAME_MAX_LEN + 1 + 24];
            char duration[24];
            format_print_time(duration, sizeof(duration), last_print_job.duration_seconds);
            /* Keep the action readable and give a long G-code filename its
             * own wrapped line instead of truncating either label. */
            snprintf(detail, sizeof(detail), "%s\n%s", last_print_job.filename, duration);
            add_nav_wrapped_card("Repeat last print", detail, ">", UI_TONE_DEFAULT,
                                 open_last_print_event, NULL);
        }
        add_nav_card("Choose file", "Browse G-code files", ">", UI_TONE_DEFAULT,
                     start_print_files_event, NULL);
        break;
    }
    case PAGE_SETTINGS:
    {
        lv_label_set_text(page_title, "Settings");
        wifi_manager_status_t wifi;
        wifi_manager_get_status(&wifi);
        size_t printer_count = 0;
        settings_get_printers(printer_list_snapshot, SETTINGS_MAX_PRINTERS, &printer_count);
        ui_kit_create_section(page_body, "CONNECTIVITY");
        add_nav_row("Network", wifi_manager_state_name(wifi.state),
                    wifi.state == WIFI_MANAGER_CONNECTED ? UI_TONE_SUCCESS : UI_TONE_WARNING,
                    true, navigation_event, (void *)PAGE_NETWORK_SETUP);
        char printer_summary[24];
        snprintf(printer_summary, sizeof(printer_summary), "%u configured", (unsigned)printer_count);
        add_nav_row("Printers", printer_summary, UI_TONE_DEFAULT, true,
                    navigation_event, (void *)PAGE_PRINTER_LIST);
        ui_kit_create_section(page_body, "DEVICE");
        char brightness[16];
        snprintf(brightness, sizeof(brightness), "%u%%", board_get_backlight());
        add_nav_row("Display", brightness, UI_TONE_ACCENT, true, brightness_event, NULL);
        settings_theme_t theme;
        settings_get_theme(&theme);
        add_nav_row("Theme", theme == SETTINGS_THEME_DARK ? "Dark" : "Light",
                    UI_TONE_ACCENT, true, theme_event, NULL);
        add_nav_row("Button mapping", NULL, UI_TONE_DEFAULT, true,
                    navigation_event, (void *)PAGE_BUTTON_MAPPING);
        add_nav_row("Battery", "Live debug", UI_TONE_DEFAULT, true,
                    navigation_event, (void *)PAGE_BATTERY);
        add_nav_row("Firmware", NULL, UI_TONE_DEFAULT, true,
                    navigation_event, (void *)PAGE_FIRMWARE);
        ui_kit_create_section(page_body, "SYSTEM");
        add_nav_row("Diagnostics", NULL, UI_TONE_DEFAULT, true,
                    navigation_event, (void *)PAGE_DIAGNOSTICS);
        break;
    }
    case PAGE_BUTTON_MAPPING:
    {
        lv_label_set_text(page_title, "Button mapping");
        lv_label_set_text(back_hint, "Tap to change  |  Keep one Back button");
        if (settings_get_button_mapping(&editable_button_mapping) != ESP_OK) break;
        ui_kit_create_section(page_body, "PHYSICAL BUTTONS");
        lv_obj_t *left_row = add_nav_row("Left button", button_action_text(editable_button_mapping.left),
                                         UI_TONE_ACCENT, true, button_mapping_event,
                                         (void *)(uintptr_t)1);
        lv_obj_t *right_row = add_nav_row("Right button", button_action_text(editable_button_mapping.right),
                                          UI_TONE_ACCENT, true, button_mapping_event, (void *)false);
        if (button_mapping_editing) {
            button_mapping_row = button_mapping_edit_left ? left_row : right_row;
            set_inline_row_visual(button_mapping_row, true);
            lv_label_set_text(back_hint, "Rotate: choose  |  Press: save  |  Back: cancel");
        }
        if (editable_button_mapping.left == SETTINGS_BUTTON_ACTION_GCODE)
            add_nav_row("Set G-code", editable_button_mapping.left_gcode[0] ? "EDIT" : "SET",
                        UI_TONE_WARNING, true, edit_field_event, (void *)EDIT_LEFT_BUTTON_GCODE);
        if (editable_button_mapping.right == SETTINGS_BUTTON_ACTION_GCODE)
            add_nav_row("Set G-code", editable_button_mapping.right_gcode[0] ? "EDIT" : "SET",
                        UI_TONE_WARNING, true, edit_field_event, (void *)EDIT_RIGHT_BUTTON_GCODE);
        ui_kit_create_section(page_body, "ACTIONS");
        add_nav_row("None", "Disabled", UI_TONE_MUTED, false, NULL, NULL);
        add_nav_row("Back", "Go back", UI_TONE_DEFAULT, false, NULL, NULL);
        add_nav_row("E-stop", "Hold to send", UI_TONE_DANGER, false, NULL, NULL);
        add_nav_row("G-code", "Hold to run", UI_TONE_WARNING, false, NULL, NULL);
        break;
    }
    case PAGE_PRINTER_LIST:
    {
        lv_label_set_text(page_title, "Printers");
        size_t count = 0;
        settings_get_printers(printer_list_snapshot, SETTINGS_MAX_PRINTERS, &count);
        size_t active_index = SIZE_MAX;
        settings_get_active_printer_index(&active_index);
        ui_kit_create_section(page_body, "CONFIGURED PRINTERS");
        for (size_t i = 0; i < count; ++i) {
            char detail[80];
            snprintf(detail, sizeof(detail), "%s:%u%s", printer_list_snapshot[i].host, printer_list_snapshot[i].port,
                     i == active_index ? "  ACTIVE" : "");
            add_nav_card(printer_list_snapshot[i].name, detail, "EDIT",
                         i == active_index ? UI_TONE_SUCCESS : UI_TONE_DEFAULT,
                         edit_printer_event, (void *)(uintptr_t)i);
        }
        if (count < SETTINGS_MAX_PRINTERS) {
            add_nav_row("Add printer", NULL, UI_TONE_ACCENT, true, add_printer_event, NULL);
        } else {
            char limit[24];
            snprintf(limit, sizeof(limit), "%u maximum", (unsigned)SETTINGS_MAX_PRINTERS);
            add_nav_row("Printer limit reached", limit, UI_TONE_MUTED, false, NULL, NULL);
        }
        break;
    }
    case PAGE_NETWORK_SETUP:
    {
        if (previous_page != PAGE_NETWORK_SETUP) load_editable_settings();
        lv_label_set_text(page_title, "Wi-Fi setup");
        lv_label_set_text(back_hint, "Save connects  |  Back: Settings");
        char item[80];
        snprintf(item, sizeof(item), "Network: %s", editable_wifi.ssid[0] ? editable_wifi.ssid : "not set");
        add_nav_button(item, edit_field_event, (void *)EDIT_WIFI_SSID);
        add_nav_button("Scan Wi-Fi networks", scan_networks_event, NULL);
        add_nav_button("Password: change", edit_field_event, (void *)EDIT_WIFI_PASSWORD);
        add_nav_button("Save and connect", save_network_event, NULL);
        wifi_manager_status_t wifi;
        wifi_manager_get_status(&wifi);
        char status[64];
        snprintf(status, sizeof(status), "Status: %s%s%s", wifi_manager_state_name(wifi.state),
                 wifi.ip[0] ? " / " : "", wifi.ip);
        add_nav_button(status, navigation_event, (void *)PAGE_NETWORK_SETUP);
        if (wifi.scanning) {
            add_nav_button("Scanning nearby networks...", navigation_event, (void *)PAGE_NETWORK_SETUP);
        } else {
            wifi_manager_network_t networks[8];
            size_t count = wifi_manager_get_networks(networks, 8);
            for (size_t i = 0; i < count; ++i) {
                snprintf(item, sizeof(item), "%s  (%d dBm)", networks[i].ssid, networks[i].rssi);
                add_nav_button(item, select_network_event, (void *)(uintptr_t)i);
            }
        }
        break;
    }
    case PAGE_PRINTER_SETUP:
    {
        lv_label_set_text(page_title, editing_printer_index == SIZE_MAX ? "Add printer" : "Edit printer");
        char item[96];
        snprintf(item, sizeof(item), "Name: %s", editable_printer.name[0] ? editable_printer.name : "not set");
        add_nav_button(item, edit_field_event, (void *)EDIT_PRINTER_NAME);
        snprintf(item, sizeof(item), "Host: %s", editable_printer.host[0] ? editable_printer.host : "not set");
        add_nav_button(item, edit_field_event, (void *)EDIT_PRINTER_HOST);
        snprintf(item, sizeof(item), "Port: %u", editable_printer.port);
        add_nav_button(item, edit_field_event, (void *)EDIT_PRINTER_PORT);
        add_nav_button("Save printer", save_printer_event, NULL);
        break;
    }
    case PAGE_DIAGNOSTICS:
        lv_label_set_text(page_title, "Hardware diagnostics");
        lv_label_set_text(back_hint, "Live status  |  Back: Settings");
        diagnostics_label = lv_label_create(page_body);
        lv_obj_set_width(diagnostics_label, LV_PCT(100));
        lv_obj_set_style_text_color(diagnostics_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
        refresh_diagnostics();
        break;
    case PAGE_BATTERY:
        lv_label_set_text(page_title, "Battery");
        lv_label_set_text(back_hint, "Live status  |  Back: Settings");
        battery_debug_label = lv_label_create(page_body);
        lv_obj_set_width(battery_debug_label, LV_PCT(100));
        lv_obj_set_style_text_color(battery_debug_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
        refresh_battery_debug();
        break;
    case PAGE_FIRMWARE:
    {
        ota_update_status_t update;
        ota_update_get_status(&update);
        lv_label_set_text(page_title, "Firmware update");
        lv_label_set_text(back_hint, "Only trusted GitHub releases are installed");
        ui_kit_create_section(page_body, "CURRENT RELEASE");
        add_nav_row("Installed", update.current_version[0] ? update.current_version : "Unknown",
                    UI_TONE_DEFAULT, false, NULL, NULL);
        ui_kit_create_section(page_body, "UPDATE");
        if (update.state == OTA_UPDATE_DOWNLOADING || update.state == OTA_UPDATE_VERIFYING ||
            update.state == OTA_UPDATE_RESTARTING) {
            char detail[24];
            snprintf(detail, sizeof(detail), "%u%%", update.progress);
            add_nav_row(ota_update_state_name(update.state), detail, UI_TONE_ACCENT, false, NULL, NULL);
            add_nav_row(update.message, NULL, UI_TONE_MUTED, false, NULL, NULL);
            add_nav_row("Controls locked until restart", NULL, UI_TONE_WARNING, false, NULL, NULL);
        } else if (update.state == OTA_UPDATE_AVAILABLE || update.state == OTA_UPDATE_UP_TO_DATE) {
            add_nav_row(ota_update_state_name(update.state), NULL, UI_TONE_SUCCESS, false, NULL, NULL);
            add_nav_row(update.message, NULL, UI_TONE_MUTED, false, NULL, NULL);
            ui_kit_create_section(page_body, "AVAILABLE RELEASES");
            for (uint8_t i = 0; i < update.release_count; ++i) {
                char label[56];
                if (strcmp(update.releases[i].version, update.current_version) == 0) {
                    add_nav_row(update.releases[i].version, "Installed", UI_TONE_MUTED, false, NULL, NULL);
                } else {
                    snprintf(label, sizeof(label), "Install %s", update.releases[i].version);
                    add_nav_hold_row(label, "HOLD", UI_TONE_WARNING, firmware_install_release_event,
                                     (void *)(uintptr_t)i);
                }
            }
            add_nav_row("Check again", NULL, UI_TONE_DEFAULT, true, firmware_check_event, NULL);
        } else {
            add_nav_row(ota_update_state_name(update.state), NULL,
                        update.state == OTA_UPDATE_ERROR ? UI_TONE_DANGER : UI_TONE_MUTED,
                        false, NULL, NULL);
            if (update.message[0]) add_nav_row(update.message, NULL, UI_TONE_MUTED, false, NULL, NULL);
            add_nav_row("Check for updates", NULL, UI_TONE_ACCENT, true, firmware_check_event, NULL);
        }
        break;
    }
    }
    lv_label_set_text(back_hint, "");
    const uint32_t child_count = lv_obj_get_child_cnt(page_body);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(page_body, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE) &&
            !lv_obj_has_state(child, LV_STATE_DISABLED)) {
            lv_group_focus_obj(child);
            break;
        }
    }
    set_bottom_navigation(page);
    /* Page content is rebuilt in a shared scrollable container.  Resetting
     * before lv_obj_clean() is not sufficient: the subsequent flex layout
     * and viewport-height change can restore a stale scroll offset. */
    lv_obj_update_layout(page_body);
    lv_obj_scroll_to(page_body, 0, 0, LV_ANIM_OFF);
}

esp_err_t pendant_ui_init(void)
{
    input_event_queue = xQueueCreate(UI_INPUT_QUEUE_LENGTH, sizeof(pendant_input_event_t));
    if (input_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) {
        vQueueDelete(input_event_queue);
        input_event_queue = NULL;
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_scr_act();
    ui_kit_init();
    settings_theme_t theme;
    settings_get_theme(&theme);
    ui_kit_set_theme(theme == SETTINGS_THEME_DARK ? UI_THEME_DARK : UI_THEME_LIGHT);
    ui_kit_apply_screen(screen);
    navigation_group = lv_group_create();

    /* The printer name is context, not a hidden menu item. */
    header_printer_button = lv_obj_create(screen);
    lv_obj_set_size(header_printer_button, 140, 38);
    lv_obj_align(header_printer_button, LV_ALIGN_TOP_LEFT, 10, 6);
    lv_obj_set_style_bg_opa(header_printer_button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_printer_button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header_printer_button, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_printer_button, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    header_connection_dot = lv_obj_create(header_printer_button);
    lv_obj_set_size(header_connection_dot, 8, 8);
    lv_obj_align(header_connection_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(header_connection_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_connection_dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_connection_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    header_printer_label = lv_label_create(header_printer_button);
    lv_label_set_long_mode(header_printer_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(header_printer_label, 120);
    lv_obj_align(header_printer_label, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(header_printer_label, &lv_font_montserrat_18, LV_PART_MAIN);
    /* The selected printer is context, not a navigation control. */
    lv_obj_set_style_text_color(header_printer_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);

    command_feedback_label = lv_label_create(screen);
    lv_obj_set_width(command_feedback_label, 205);
    lv_obj_align(command_feedback_label, LV_ALIGN_TOP_LEFT, 10, 34);
    lv_label_set_long_mode(command_feedback_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(command_feedback_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_add_flag(command_feedback_label, LV_OBJ_FLAG_HIDDEN);
    command_feedback_timer = lv_timer_create(command_feedback_timer_cb, 3500, NULL);
    lv_timer_pause(command_feedback_timer);
    lv_timer_t *input_event_timer = lv_timer_create(input_event_timer_cb, 5, NULL);
    if (input_event_timer == NULL) {
        lvgl_port_unlock();
        vQueueDelete(input_event_queue);
        input_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    page_title = lv_label_create(screen);
    lv_obj_set_style_text_color(page_title, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
    lv_obj_set_style_text_font(page_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(page_title, LV_ALIGN_TOP_LEFT, 12, 57);
    lv_obj_set_width(page_title, 296);
    lv_obj_set_height(page_title, 24);
    lv_label_set_long_mode(page_title, LV_LABEL_LONG_DOT);

    battery_label = lv_label_create(screen);
    /* 100% plus both Font Awesome glyphs needs more than 64 px at 14 pt. */
    lv_obj_set_width(battery_label, 84);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(battery_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -10, 17);
    wifi_signal_icon = lv_obj_create(screen);
    lv_obj_set_size(wifi_signal_icon, 24, 16);
    lv_obj_align(wifi_signal_icon, LV_ALIGN_TOP_RIGHT, -112, 17);
    lv_obj_set_style_bg_opa(wifi_signal_icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_signal_icon, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_signal_icon, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_signal_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    static const int16_t bar_heights[] = { 5, 10, 15 };
    for (uint8_t i = 0; i < 3; ++i) {
        wifi_signal_bars[i] = lv_obj_create(wifi_signal_icon);
        lv_obj_set_size(wifi_signal_bars[i], 5, bar_heights[i]);
        lv_obj_align(wifi_signal_bars[i], LV_ALIGN_BOTTOM_LEFT, i * 8, 0);
        lv_obj_set_style_radius(wifi_signal_bars[i], 1, LV_PART_MAIN);
        lv_obj_set_style_border_width(wifi_signal_bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(wifi_signal_bars[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    battery_timer_cb(NULL);
    lv_timer_create(battery_timer_cb, 1000, NULL);

    lv_obj_t *header_divider = lv_obj_create(screen);
    lv_obj_set_size(header_divider, 320, 1);
    lv_obj_align(header_divider, LV_ALIGN_TOP_MID, 0, 49);
    lv_obj_set_style_bg_color(header_divider, ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_divider, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_divider, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    page_body = lv_obj_create(screen);
    lv_obj_set_size(page_body, 300, 324);
    lv_obj_align(page_body, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(page_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(page_body, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(page_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(page_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page_body, 7, LV_PART_MAIN);

    /* Fixed page controls for long file listings.  They sit above the
     * scrollable page_body so both arrows and the page indicator stay in
     * view while browsing file cards. */
    files_pagination = lv_obj_create(screen);
    lv_obj_set_size(files_pagination, 300, 30);
    lv_obj_align(files_pagination, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(files_pagination, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(files_pagination, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(files_pagination, 0, LV_PART_MAIN);
    lv_obj_clear_flag(files_pagination, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(files_pagination, LV_OBJ_FLAG_HIDDEN);

    files_previous_page_button = lv_btn_create(files_pagination);
    files_next_page_button = lv_btn_create(files_pagination);
    lv_obj_t *page_buttons[] = { files_previous_page_button, files_next_page_button };
    for (size_t i = 0; i < sizeof(page_buttons) / sizeof(page_buttons[0]); ++i) {
        lv_obj_t *button = page_buttons[i];
        lv_obj_set_size(button, 42, 30);
        lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, ui_kit_color(UI_COLOR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(button, ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                      LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(button, ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                       LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(button, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_event_cb(button, touch_focus_event, LV_EVENT_PRESSED, NULL);
    }
    lv_obj_align(files_previous_page_button, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(files_next_page_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(files_previous_page_button, files_previous_page_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(files_next_page_button, files_next_page_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *previous_label = lv_label_create(files_previous_page_button);
    lv_label_set_text(previous_label, LV_SYMBOL_LEFT);
    lv_obj_center(previous_label);
    lv_obj_t *next_label = lv_label_create(files_next_page_button);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);
    files_page_label = lv_label_create(files_pagination);
    lv_obj_set_width(files_page_label, 100);
    lv_obj_set_style_text_align(files_page_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(files_page_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(files_page_label, ui_kit_tone_color(UI_TONE_DEFAULT), LV_PART_MAIN);
    lv_obj_align(files_page_label, LV_ALIGN_CENTER, 0, 0);

    back_hint = lv_label_create(screen);
    lv_obj_set_style_text_color(back_hint, ui_kit_tone_color(UI_TONE_MUTED), LV_PART_MAIN);
    lv_obj_align(back_hint, LV_ALIGN_BOTTOM_LEFT, 12, -68);
    lv_obj_set_width(back_hint, 296);
    lv_label_set_long_mode(back_hint, LV_LABEL_LONG_DOT);

    static const char *nav_labels[] = {
        LV_SYMBOL_HOME "\nMonitor", LV_SYMBOL_SETTINGS "\nTune", LV_SYMBOL_RIGHT "\nMove",
        LV_SYMBOL_FILE "\nData", LV_SYMBOL_LIST "\nService"
    };
    static const page_t nav_pages[] = { PAGE_STATUS, PAGE_TUNE, PAGE_MOVE, PAGE_DATA, PAGE_SERVICE };
    for (size_t i = 0; i < sizeof(nav_pages) / sizeof(nav_pages[0]); ++i) {
        bottom_navigation[i] = lv_btn_create(screen);
        lv_obj_set_size(bottom_navigation[i], 60, 54);
        lv_obj_align(bottom_navigation[i], LV_ALIGN_BOTTOM_LEFT, 8 + (int32_t)i * 61, -6);
        lv_obj_set_style_radius(bottom_navigation[i], 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(bottom_navigation[i], ui_kit_color(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(bottom_navigation[i], 2, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(bottom_navigation[i], ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                      LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(bottom_navigation[i], ui_kit_color(UI_COLOR_BORDER_FOCUSED),
                                       LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(bottom_navigation[i], 2, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(bottom_navigation[i], 2, LV_PART_MAIN);
        lv_obj_add_event_cb(bottom_navigation[i], touch_focus_event, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(bottom_navigation[i], navigation_event, LV_EVENT_CLICKED, (void *)nav_pages[i]);
        lv_obj_t *label = lv_label_create(bottom_navigation[i]);
        lv_label_set_text(label, nav_labels[i]);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(label);
    }

    navigate_to(PAGE_FLEET);
    lvgl_port_unlock();
    return ESP_OK;
}

static void handle_input_locked(const pendant_input_event_t *event)
{
    pendant_input_event_t mapped_event;
    /* Flash writes and signature verification must run without a competing
     * navigation or printer action.  The update page remains informational
     * and is refreshed by the regular UI timer. */
    if (ota_update_is_installing()) return;
    if (current_page == PAGE_FIRMWARE) {
        ota_update_status_t update;
        ota_update_get_status(&update);
        if (update.state == OTA_UPDATE_CHECKING) return;
    }
    if (event->type == PENDANT_INPUT_LEFT || event->type == PENDANT_INPUT_LEFT_HOLD ||
        event->type == PENDANT_INPUT_RIGHT || event->type == PENDANT_INPUT_RIGHT_HOLD) {
        settings_button_mapping_t mapping;
        if (settings_get_button_mapping(&mapping) != ESP_OK) return;
        const bool left = event->type == PENDANT_INPUT_LEFT || event->type == PENDANT_INPUT_LEFT_HOLD;
        const bool held = event->type == PENDANT_INPUT_LEFT_HOLD || event->type == PENDANT_INPUT_RIGHT_HOLD;
        const settings_button_action_t action = left ? mapping.left : mapping.right;
        if (action == SETTINGS_BUTTON_ACTION_NONE) return;
        if (action == SETTINGS_BUTTON_ACTION_GCODE) {
            const char *script = left ? mapping.left_gcode : mapping.right_gcode;
            if (held && script[0] != '\0' && is_printer_page(current_page))
                report_command_result("Button G-code", moonraker_send_gcode(script));
            return;
        }
        mapped_event = *event;
        if (action == SETTINGS_BUTTON_ACTION_BACK)
            mapped_event.type = held ? PENDANT_INPUT_BACK_HOLD : PENDANT_INPUT_BACK;
        else
            mapped_event.type = held ? PENDANT_INPUT_ESTOP_HOLD : PENDANT_INPUT_ESTOP;
        event = &mapped_event;
    }
    if (event->type == PENDANT_INPUT_BACK_HOLD) {
        /* A modal editor belongs to the current page, not to Fleet.  Close it
         * before changing pages so its keyboard cannot remain above Fleet. */
        if (editor_overlay != NULL) editor_close();
        if (tune_mode != TUNE_MODE_BROWSE) tune_cancel_event(NULL);
        else if (move_mode != MOVE_MODE_BROWSE) move_cancel_event(NULL);
        navigate_to(PAGE_FLEET);
        return;
    }
    /* E-STOP has no modal and is deliberately scoped to an already selected
     * printer.  It remains a network command, not a safety-rated hard stop. */
    if (event->type == PENDANT_INPUT_ESTOP_HOLD) {
        if (is_printer_page(current_page)) report_command_result("E-STOP", moonraker_emergency_stop());
        return;
    }
    if (event->type == PENDANT_INPUT_ESTOP) return;
    if (current_page == PAGE_BUTTON_MAPPING && button_mapping_editing) {
        settings_button_action_t *action = button_mapping_edit_left ?
            &editable_button_mapping.left : &editable_button_mapping.right;
        if (event->type == PENDANT_INPUT_ENCODER_ROTATE) {
            const int count = SETTINGS_BUTTON_ACTION_GCODE + 1;
            *action = (settings_button_action_t)((*action + count + event->delta) % count);
            if (button_mapping_row != NULL) {
                ui_kit_set_row_text(button_mapping_row,
                                    button_mapping_edit_left ? "Left button" : "Right button",
                                    button_action_text(*action));
            }
        } else if (event->type == PENDANT_INPUT_ENCODER_PRESS ||
                   event->type == PENDANT_INPUT_ENCODER_HOLD) {
            if (settings_set_button_mapping(&editable_button_mapping) != ESP_OK) {
                report_command_result("Keep one Back button", ESP_FAIL);
                return;
            }
            button_mapping_editing = false;
            navigate_to(PAGE_BUTTON_MAPPING);
        } else if (event->type == PENDANT_INPUT_BACK) {
            *action = button_mapping_original_action;
            button_mapping_editing = false;
            navigate_to(PAGE_BUTTON_MAPPING);
        }
        return;
    }
    /* Down/up remain available for a future feedback layer. */
    if (event->type == PENDANT_INPUT_ENCODER_DOWN) return;
    if (current_page == PAGE_TUNE && tune_mode != TUNE_MODE_BROWSE) {
        if (event->type == PENDANT_INPUT_ENCODER_ROTATE) {
            int32_t step = tune_mode == TUNE_MODE_OFFSET ? 1 :
                           tune_mode == TUNE_MODE_HEATER ? 10 : 1;
            if (tune_mode != TUNE_MODE_OFFSET) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t interval_us = tune_last_rotate_us == 0 ? INT64_MAX :
                    now_us - tune_last_rotate_us;
                if (interval_us <= 50000) step *= 5;
                tune_last_rotate_us = now_us;
            }
            int64_t candidate = (int64_t)tune_value + (int64_t)step * event->delta;
            int32_t minimum = tune_mode == TUNE_MODE_OFFSET ? -500 :
                              tune_mode == TUNE_MODE_FLOW ? 1 : 0;
            int32_t maximum = tune_mode == TUNE_MODE_OFFSET ? 500 :
                              tune_mode == TUNE_MODE_FAN ? 100 : 300;
            if (tune_mode == TUNE_MODE_HEATER) {
                minimum = tune_capabilities.heaters[tune_item_index].min_deci_c;
                maximum = tune_capabilities.heaters[tune_item_index].max_deci_c;
            }
            if (candidate < minimum) candidate = minimum;
            if (candidate > maximum) candidate = maximum;
            tune_value = (int32_t)candidate;
            tune_update_value_row();
        } else if (event->type == PENDANT_INPUT_ENCODER_HOLD) {
            tune_confirm_event(NULL);
        } else if (event->type == PENDANT_INPUT_BACK) {
            tune_cancel_event(NULL);
        }
        return;
    }
    if (current_page == PAGE_MOVE && move_mode != MOVE_MODE_BROWSE) {
        if (event->type == PENDANT_INPUT_ENCODER_ROTATE) {
            if (move_mode == MOVE_MODE_AXIS || move_mode == MOVE_MODE_EXTRUDER) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t interval_us = move_last_rotate_us == 0 ? INT64_MAX :
                    now_us - move_last_rotate_us;
                int32_t multiplier = 1;
                if (interval_us <= 25000) multiplier = 20;
                else if (interval_us <= 50000) multiplier = 10;
                else if (interval_us <= 100000) multiplier = 5;
                else if (interval_us <= 180000) multiplier = 2;
                move_last_rotate_us = now_us;

                int64_t candidate = (int64_t)move_distance_centi_mm +
                    (int64_t)move_steps_centi_mm[move_step_index] * event->delta * multiplier;
                /* Clamp instead of discarding the final accelerated detent,
                 * so direction changes at either limit remain predictable. */
                const int32_t limit = move_mode == MOVE_MODE_EXTRUDER ? 10000 : 99999;
                if (candidate < -limit) candidate = -limit;
                if (candidate > limit) candidate = limit;
                move_distance_centi_mm = (int32_t)candidate;
            } else if (move_mode == MOVE_MODE_EXTRUDER_SPEED) {
                int64_t candidate = (int64_t)move_extruder_speed_mm_s + event->delta;
                if (candidate < 1) candidate = 1;
                if (candidate > 50) candidate = 50;
                move_extruder_speed_mm_s = (int32_t)candidate;
            } else if (move_mode == MOVE_MODE_STEP) {
                const int count = (int)(sizeof(move_steps_centi_mm) / sizeof(move_steps_centi_mm[0]));
                move_step_index = (uint8_t)((move_step_index + count + event->delta) % count);
            } else {
                const int count = (int)(sizeof(move_park_labels) / sizeof(move_park_labels[0]));
                move_park_index = (uint8_t)((move_park_index + count + event->delta) % count);
            }
            move_update_value_row();
        } else if (event->type == PENDANT_INPUT_ENCODER_HOLD) {
            move_confirm_event(NULL);
        } else if (event->type == PENDANT_INPUT_BACK) {
            move_cancel_event(NULL);
        }
        return;
    }
    if (event->type == PENDANT_INPUT_ENCODER_ROTATE) {
        if (event->delta > 0) {
            lv_group_focus_next(navigation_group);
        } else {
            lv_group_focus_prev(navigation_group);
        }
    } else if (event->type == PENDANT_INPUT_ENCODER_HOLD) {
        lv_obj_t *focused = lv_group_get_focused(navigation_group);
        if (focused != NULL) {
            lv_event_send(focused, LV_EVENT_LONG_PRESSED, NULL);
        }
    } else if (event->type == PENDANT_INPUT_ENCODER_PRESS) {
        lv_obj_t *focused = lv_group_get_focused(navigation_group);
        if (focused != NULL) {
            lv_event_send(focused, LV_EVENT_CLICKED, NULL);
        }
    } else if (event->type == PENDANT_INPUT_BACK) {
        if (editor_overlay != NULL) {
            editor_close();
        } else if (current_page == PAGE_PROMPT) {
            moonraker_dismiss_prompt();
            navigate_to(prompt_return_page);
        } else if (current_page == PAGE_FILES) {
            moonraker_get_file_browser(&file_browser_snapshot);
            if (strcmp(file_browser_snapshot.path, "gcodes") != 0) open_parent_directory();
            else navigate_to(PAGE_STATUS);
        } else if (current_page == PAGE_FILE_DETAILS) {
            navigate_to(PAGE_FILES);
        } else if (current_page == PAGE_DATA) {
            /* Data is a top-level printer tab, so its Back target is Monitor,
             * like Tune, Move, and Service—not the global Fleet. */
            navigate_to(PAGE_STATUS);
        } else if (current_page == PAGE_MACROS || current_page == PAGE_START_PRINT ||
                   current_page == PAGE_EXCLUDE_OBJECTS || current_page == PAGE_EXCLUDE_CONFIRM ||
                   current_page == PAGE_HISTORY || current_page == PAGE_HISTORY_DETAILS ||
                   current_page == PAGE_CONSOLE || current_page == PAGE_SYSTEM_INFO ||
                   current_page == PAGE_PROBE_CALIBRATE) {
            navigate_to(parent_page(current_page));
        } else if (current_page == PAGE_STATUS) {
            navigate_to(PAGE_FLEET);
        } else if (current_page != PAGE_FLEET) {
            /* All printer sections are peers beneath Monitor. Configuration
             * screens retain their explicit local parent. */
            navigate_to(is_printer_page(current_page) ? PAGE_STATUS : parent_page(current_page));
        }
    }
}

void pendant_ui_handle_input(const pendant_input_event_t *event)
{
    if (event == NULL || input_event_queue == NULL ||
        (event->type != PENDANT_INPUT_ENCODER_ROTATE &&
         event->type != PENDANT_INPUT_ENCODER_DOWN &&
         event->type != PENDANT_INPUT_ENCODER_PRESS && !event->pressed)) {
        return;
    }

    /* Queueing is thread-safe and non-blocking.  In particular, it leaves
     * pendant_input free to drain every GPIO edge while Monitor is redrawing. */
    (void)xQueueSend(input_event_queue, event, 0);
}
