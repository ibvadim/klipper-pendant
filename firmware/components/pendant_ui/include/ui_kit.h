#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef enum {
    UI_TONE_DEFAULT,
    UI_TONE_ACCENT,
    UI_TONE_SUCCESS,
    UI_TONE_WARNING,
    UI_TONE_DANGER,
    UI_TONE_MUTED,
} ui_tone_t;

typedef enum {
    UI_THEME_LIGHT,
    UI_THEME_DARK,
} ui_theme_t;

/* The complete approved palette.  Widgets must select a semantic token,
 * never embed a literal RGB value. */
typedef enum {
    UI_COLOR_BG,
    UI_COLOR_SURFACE,
    UI_COLOR_SURFACE_2,
    UI_COLOR_BORDER,
    UI_COLOR_SURFACE_FOCUSED,
    UI_COLOR_BORDER_FOCUSED,
    UI_COLOR_TEXT_PRIMARY,
    UI_COLOR_TEXT_MUTED,
    UI_COLOR_TEXT_DISABLED,
    UI_COLOR_ACCENT,
    UI_COLOR_STATUS_OK,
    UI_COLOR_STATUS_WARN,
    UI_COLOR_STATUS_DANGER,
} ui_color_token_t;

typedef struct {
    const char *title;
    const char *subtitle;
    const char *value;
    ui_tone_t tone;
    bool enabled;
} ui_row_spec_t;

/** Initializes the shared LVGL styles. Safe to call more than once. */
void ui_kit_init(void);
void ui_kit_set_theme(ui_theme_t theme);
ui_theme_t ui_kit_get_theme(void);

/** Applies the pendant background and default text color to a screen. */
void ui_kit_apply_screen(lv_obj_t *screen);

/** Creates a non-focusable section heading inside a scrolling content area. */
lv_obj_t *ui_kit_create_section(lv_obj_t *parent, const char *title);

/** Creates a row and, when enabled, adds it to the shared navigation group. */
lv_obj_t *ui_kit_create_row(lv_obj_t *parent, lv_group_t *group,
                            const ui_row_spec_t *spec,
                            lv_event_cb_t callback, void *user_data);

/** Creates a card with a wrapped, full-width title and content-based height.
 * Unlike a regular row, focusing it never starts a marquee. */
lv_obj_t *ui_kit_create_wrapped_row(lv_obj_t *parent, lv_group_t *group,
                                    const ui_row_spec_t *spec,
                                    lv_event_cb_t callback, void *user_data);

/** Updates the two textual fields of a row created by ui_kit_create_row. */
void ui_kit_set_row_text(lv_obj_t *row, const char *title, const char *value);

/** Updates the optional secondary line and expands the row when needed. */
void ui_kit_set_row_subtitle(lv_obj_t *row, const char *subtitle);

/** Changes the semantic color of a row's trailing value. */
void ui_kit_set_row_tone(lv_obj_t *row, ui_tone_t tone);

/** Returns the semantic color used by labels and screen-specific widgets. */
lv_color_t ui_kit_tone_color(ui_tone_t tone);

/** Resolves a palette token for the currently selected light/dark theme. */
lv_color_t ui_kit_color(ui_color_token_t token);
