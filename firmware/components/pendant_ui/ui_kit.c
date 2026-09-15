#include "ui_kit.h"

enum {
    DARK_BG = 0x141414, DARK_SURFACE = 0x1e1e1e, DARK_SURFACE_2 = 0x262626,
    DARK_BORDER = 0x333333, DARK_SURFACE_FOCUSED = 0x3a2c08,
    DARK_BORDER_FOCUSED = 0xe2a233, DARK_TEXT_PRIMARY = 0xf2f2f2,
    DARK_TEXT_MUTED = 0x9a9a9a, DARK_TEXT_DISABLED = 0x5c5c5c,
    DARK_ACCENT = 0xff6a1a, DARK_STATUS_OK = 0x3fcb6e,
    DARK_STATUS_WARN = 0xe0a020, DARK_STATUS_DANGER = 0xff5c5c,
    LIGHT_BG = 0xf4f4f5, LIGHT_SURFACE = 0xffffff, LIGHT_SURFACE_2 = 0xececed,
    LIGHT_BORDER = 0xd6d6d8, LIGHT_SURFACE_FOCUSED = 0xffe9b3,
    LIGHT_BORDER_FOCUSED = 0xc97a00, LIGHT_TEXT_PRIMARY = 0x1a1a1a,
    LIGHT_TEXT_MUTED = 0x6b6b6e, LIGHT_TEXT_DISABLED = 0xb0b0b2,
    LIGHT_ACCENT = 0xff6a1a, LIGHT_STATUS_OK = 0x1e9e52,
    LIGHT_STATUS_WARN = 0xc4780c, LIGHT_STATUS_DANGER = 0xd6293e,
};

static bool initialized;
static ui_theme_t active_theme = UI_THEME_LIGHT;
static lv_style_t row_style;
static lv_style_t row_focused_style;
static lv_style_t info_style;
static lv_style_t notice_style;

lv_color_t ui_kit_color(ui_color_token_t token)
{
    static const uint32_t dark[] = {
        DARK_BG, DARK_SURFACE, DARK_SURFACE_2, DARK_BORDER, DARK_SURFACE_FOCUSED,
        DARK_BORDER_FOCUSED, DARK_TEXT_PRIMARY, DARK_TEXT_MUTED, DARK_TEXT_DISABLED,
        DARK_ACCENT, DARK_STATUS_OK, DARK_STATUS_WARN, DARK_STATUS_DANGER,
    };
    static const uint32_t light[] = {
        LIGHT_BG, LIGHT_SURFACE, LIGHT_SURFACE_2, LIGHT_BORDER, LIGHT_SURFACE_FOCUSED,
        LIGHT_BORDER_FOCUSED, LIGHT_TEXT_PRIMARY, LIGHT_TEXT_MUTED, LIGHT_TEXT_DISABLED,
        LIGHT_ACCENT, LIGHT_STATUS_OK, LIGHT_STATUS_WARN, LIGHT_STATUS_DANGER,
    };
    return lv_color_hex((active_theme == UI_THEME_DARK ? dark : light)[token]);
}

lv_color_t ui_kit_tone_color(ui_tone_t tone)
{
    switch (tone) {
    case UI_TONE_ACCENT: return ui_kit_color(UI_COLOR_ACCENT);
    case UI_TONE_SUCCESS: return ui_kit_color(UI_COLOR_STATUS_OK);
    case UI_TONE_WARNING: return ui_kit_color(UI_COLOR_STATUS_WARN);
    case UI_TONE_DANGER: return ui_kit_color(UI_COLOR_STATUS_DANGER);
    case UI_TONE_MUTED: return ui_kit_color(UI_COLOR_TEXT_MUTED);
    case UI_TONE_DEFAULT:
    default: return ui_kit_color(UI_COLOR_TEXT_PRIMARY);
    }
}

void ui_kit_init(void)
{
    if (initialized) return;

    lv_style_init(&row_style);
    lv_style_set_bg_color(&row_style, ui_kit_color(UI_COLOR_SURFACE));
    lv_style_set_bg_opa(&row_style, LV_OPA_COVER);
    lv_style_set_border_color(&row_style, ui_kit_color(UI_COLOR_BORDER));
    lv_style_set_border_width(&row_style, 1);
    lv_style_set_radius(&row_style, 6);
    lv_style_set_pad_left(&row_style, 12);
    lv_style_set_pad_right(&row_style, 12);
    lv_style_set_pad_top(&row_style, 0);
    lv_style_set_pad_bottom(&row_style, 0);

    lv_style_init(&row_focused_style);
    lv_style_set_bg_color(&row_focused_style, ui_kit_color(UI_COLOR_SURFACE_FOCUSED));
    /* Border/outline now use a warm amber rather than a second shade of
     * blue, so focus stays visible even if blue is the channel that skews
     * under a bad viewing angle. Width bumped so the edge survives blur. */
    lv_style_set_border_color(&row_focused_style, ui_kit_color(UI_COLOR_BORDER_FOCUSED));
    lv_style_set_border_width(&row_focused_style, 2);
    lv_style_set_outline_color(&row_focused_style, ui_kit_color(UI_COLOR_BORDER_FOCUSED));
    lv_style_set_outline_width(&row_focused_style, 2);
    lv_style_set_outline_pad(&row_focused_style, 0);

    lv_style_init(&info_style);
    lv_style_set_bg_opa(&info_style, LV_OPA_TRANSP);
    lv_style_set_border_width(&info_style, 0);
    lv_style_set_pad_left(&info_style, 12);
    lv_style_set_pad_right(&info_style, 12);

    lv_style_init(&notice_style);
    lv_style_set_bg_color(&notice_style, ui_kit_color(UI_COLOR_SURFACE_2));
    lv_style_set_border_color(&notice_style, ui_kit_color(UI_COLOR_BORDER));
    lv_style_set_border_width(&notice_style, 1);
    lv_style_set_radius(&notice_style, 6);
    lv_style_set_pad_left(&notice_style, 12);
    lv_style_set_pad_right(&notice_style, 12);

    initialized = true;
}

void ui_kit_set_theme(ui_theme_t theme)
{
    active_theme = theme;
    lv_style_set_bg_color(&row_style, ui_kit_color(UI_COLOR_SURFACE));
    lv_style_set_border_color(&row_style, ui_kit_color(UI_COLOR_BORDER));
    lv_style_set_bg_color(&row_focused_style, ui_kit_color(UI_COLOR_SURFACE_FOCUSED));
    lv_style_set_border_color(&row_focused_style, ui_kit_color(UI_COLOR_BORDER_FOCUSED));
    lv_style_set_outline_color(&row_focused_style, ui_kit_color(UI_COLOR_BORDER_FOCUSED));
    lv_style_set_bg_color(&notice_style, ui_kit_color(UI_COLOR_SURFACE_2));
    lv_style_set_border_color(&notice_style, ui_kit_color(UI_COLOR_BORDER));
}

ui_theme_t ui_kit_get_theme(void)
{
    return active_theme;
}

void ui_kit_apply_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, ui_kit_color(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, ui_kit_color(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
}

lv_obj_t *ui_kit_create_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, title == NULL ? "" : title);
    lv_obj_set_width(label, LV_PCT(100));
    /* Detail pages use this for file names.  A single unbounded line can
     * extend outside the 320 px screen and makes every following row look
     * misaligned. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(label, ui_kit_color(UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 4, LV_PART_MAIN);
    return label;
}

static void scroll_focused_row_event(lv_event_t *event)
{
    /* Focus can advance quickly while the rotary encoder is turned.  Queued
     * scroll animations then compete with page rebuilds and made the UI stop
     * responding on long lists.  Keep the focused item visible immediately
     * instead of scheduling an animation for every encoder detent. */
    lv_obj_scroll_to_view(lv_event_get_target(event), LV_ANIM_OFF);
    lv_obj_t *title = lv_obj_get_child(lv_event_get_target(event), 0);
    if (title != NULL) lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

static void stop_row_marquee_event(lv_event_t *event)
{
    lv_obj_t *title = lv_obj_get_child(lv_event_get_target(event), 0);
    if (title != NULL) {
        /* Live values must remain readable; scroll only when they overflow. */
        lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_invalidate(title);
    }
}

static void scroll_focused_wrapped_row_event(lv_event_t *event)
{
    /* File names are shown in full across multiple lines, so focus should
     * reveal the card without replacing that text with a marquee. */
    lv_obj_scroll_to_view(lv_event_get_target(event), LV_ANIM_OFF);
}

/* Pointer and encoder must advance the very same focus cursor.  LVGL's
 * pointer click alone does not make an object the group's focused object. */
static void touch_focus_row_event(lv_event_t *event)
{
    lv_group_t *group = lv_event_get_user_data(event);
    if (group != NULL) lv_group_focus_obj(lv_event_get_target(event));
}

static bool item_is_interactive(ui_item_kind_t kind)
{
    return kind == UI_ITEM_NAVIGATION || kind == UI_ITEM_ACTION ||
           kind == UI_ITEM_HOLD_ACTION || kind == UI_ITEM_INLINE;
}

static const char *item_value(const ui_item_spec_t *spec)
{
    if (spec->kind == UI_ITEM_HOLD_ACTION && (spec->value == NULL || spec->value[0] == '\0')) return LV_SYMBOL_PLAY;
    return spec->value == NULL ? "" : spec->value;
}

/* Keep both texts fully visible whenever their rendered glyph widths fit in
 * one row.  Falling back to equal columns makes the overflow rule predictable:
 * a label scrolls only after it grows beyond half the available row width. */
static void layout_item_text(lv_obj_t *row, lv_obj_t *title, lv_obj_t *value)
{
    if (row == NULL || title == NULL || value == NULL) return;

    const char *title_text = lv_label_get_text(title);
    const char *value_text = lv_label_get_text(value);
    const lv_coord_t available = lv_obj_get_content_width(row);
    if (value_text[0] == '\0') {
        lv_obj_set_width(title, available);
        return;
    }

    const lv_font_t *title_font = lv_obj_get_style_text_font(title, LV_PART_MAIN);
    const lv_font_t *value_font = lv_obj_get_style_text_font(value, LV_PART_MAIN);
    const lv_coord_t title_width = lv_txt_get_width(title_text, strlen(title_text), title_font,
        lv_obj_get_style_text_letter_space(title, LV_PART_MAIN), LV_TEXT_FLAG_NONE);
    const lv_coord_t value_width = lv_txt_get_width(value_text, strlen(value_text), value_font,
        lv_obj_get_style_text_letter_space(value, LV_PART_MAIN), LV_TEXT_FLAG_NONE);
    const lv_coord_t gap = 8;

    if (title_width + value_width + gap <= available) {
        lv_obj_set_width(title, title_width);
        lv_obj_set_width(value, value_width);
    } else {
        lv_obj_set_width(title, (available - gap) / 2);
        lv_obj_set_width(value, (available - gap) / 2);
    }
}

lv_obj_t *ui_kit_create_item(lv_obj_t *parent, lv_group_t *group,
                             const ui_item_spec_t *spec,
                             lv_event_cb_t callback, void *user_data)
{
    if (parent == NULL || spec == NULL) return NULL;

    const bool interactive = item_is_interactive(spec->kind);
    lv_obj_t *row = interactive ? lv_btn_create(parent) : lv_obj_create(parent);
    /* Do not create orphaned labels on the active screen when LVGL cannot
     * allocate a row.  Apart from looking corrupted, those labels would
     * remain after the next page refresh. */
    if (row == NULL) return NULL;
    const bool has_subtitle = spec->subtitle != NULL && spec->subtitle[0] != '\0';
    lv_obj_set_size(row, LV_PCT(100), has_subtitle ? 66 : 48);
    lv_obj_add_style(row, interactive ? &row_style :
                     (spec->kind == UI_ITEM_INFO ? &info_style : &notice_style), LV_PART_MAIN);
    if (spec->kind == UI_ITEM_NOTICE || spec->kind == UI_ITEM_UNAVAILABLE) {
        lv_obj_set_style_border_color(row, ui_kit_tone_color(spec->tone), LV_PART_MAIN);
    }
    if (interactive) {
        lv_obj_add_style(row, &row_focused_style, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_event_cb(row, scroll_focused_row_event, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(row, stop_row_marquee_event, LV_EVENT_DEFOCUSED, NULL);
        if (group != NULL) lv_obj_add_event_cb(row, touch_focus_row_event, LV_EVENT_PRESSED, group);
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title = lv_label_create(row);
    if (title == NULL) {
        lv_obj_del(row);
        return NULL;
    }
    lv_label_set_text(title, spec->title == NULL ? "" : spec->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    const char *value_text = item_value(spec);
    lv_obj_set_height(title, 22);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, has_subtitle ? -10 : 0);
    lv_obj_set_style_text_color(title, spec->kind == UI_ITEM_INFO ? ui_kit_color(UI_COLOR_TEXT_MUTED) :
                                (spec->kind == UI_ITEM_NOTICE ||
                                 ((spec->kind == UI_ITEM_ACTION || spec->kind == UI_ITEM_HOLD_ACTION) &&
                                  spec->tone != UI_TONE_DEFAULT) ? ui_kit_tone_color(spec->tone) :
                                 ui_kit_color(UI_COLOR_TEXT_PRIMARY)), LV_PART_MAIN);

    lv_obj_t *value = lv_label_create(row);
    if (value == NULL) {
        lv_obj_del(row);
        return NULL;
    }
    lv_label_set_text(value, value_text);
    lv_label_set_long_mode(value, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_height(value, 22);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, has_subtitle ? -10 : 0);
    lv_obj_set_style_text_color(value, spec->kind == UI_ITEM_INFO && spec->tone == UI_TONE_DEFAULT ?
                                ui_kit_color(UI_COLOR_TEXT_PRIMARY) :
                                (spec->kind == UI_ITEM_INLINE ? ui_kit_color(UI_COLOR_ACCENT) :
                                 ui_kit_tone_color(spec->tone)), LV_PART_MAIN);

    lv_obj_t *subtitle = lv_label_create(row);
    if (subtitle == NULL) {
        lv_obj_del(row);
        return NULL;
    }
    lv_label_set_text(subtitle, has_subtitle ? spec->subtitle : "");
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_obj_set_height(subtitle, 18);
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_LEFT, 0, -7);
    lv_obj_set_style_text_color(subtitle, ui_kit_color(UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, LV_PART_MAIN);

    lv_obj_update_layout(row);
    layout_item_text(row, title, value);

    if (interactive) {
        if (callback != NULL) lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
        if (group != NULL) lv_group_add_obj(group, row);
    }
    return row;
}

lv_obj_t *ui_kit_create_wrapped_item(lv_obj_t *parent, lv_group_t *group,
                                     const ui_item_spec_t *spec,
                                     lv_event_cb_t callback, void *user_data)
{
    if (parent == NULL || spec == NULL) return NULL;

    const bool interactive = item_is_interactive(spec->kind);
    lv_obj_t *row = interactive ? lv_btn_create(parent) : lv_obj_create(parent);
    if (row == NULL) return NULL;
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_add_style(row, interactive ? &row_style :
                     (spec->kind == UI_ITEM_INFO ? &info_style : &notice_style), LV_PART_MAIN);
    if (spec->kind == UI_ITEM_NOTICE || spec->kind == UI_ITEM_UNAVAILABLE) {
        lv_obj_set_style_border_color(row, ui_kit_tone_color(spec->tone), LV_PART_MAIN);
    }
    if (interactive) {
        lv_obj_add_style(row, &row_focused_style, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_event_cb(row, scroll_focused_wrapped_row_event, LV_EVENT_FOCUSED, NULL);
        if (group != NULL) lv_obj_add_event_cb(row, touch_focus_row_event, LV_EVENT_PRESSED, group);
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title = lv_label_create(row);
    lv_obj_t *value = lv_label_create(row);
    lv_obj_t *subtitle = lv_label_create(row);
    if (title == NULL || value == NULL || subtitle == NULL) {
        lv_obj_del(row);
        return NULL;
    }

    lv_label_set_text(title, spec->title == NULL ? "" : spec->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    /* Reserve a narrow trailing column for the disclosure indicator. */
    lv_obj_set_width(title, LV_PCT(88));
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);
    lv_obj_set_style_text_color(title, ui_kit_color(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);

    lv_label_set_text(value, item_value(spec));
    lv_obj_set_width(value, LV_PCT(10));
    lv_obj_set_height(value, 22);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, ui_kit_tone_color(spec->tone), LV_PART_MAIN);

    const bool has_subtitle = spec->subtitle != NULL && spec->subtitle[0] != '\0';
    lv_label_set_text(subtitle, has_subtitle ? spec->subtitle : "");
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_obj_set_height(subtitle, LV_SIZE_CONTENT);
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_LEFT, 0, -7);
    lv_obj_set_style_text_color(subtitle, ui_kit_color(UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, LV_PART_MAIN);

    /* The label measures itself after its constrained width is laid out. */
    lv_obj_update_layout(row);
    const lv_coord_t title_height = lv_obj_get_height(title);
    const lv_coord_t subtitle_height = has_subtitle ? lv_obj_get_height(subtitle) : 0;
    /* Both labels may wrap.  Size the card from their measured heights so a
     * long result/date/duration line never overlaps or clips the filename. */
    const lv_coord_t content_height = title_height + subtitle_height + (has_subtitle ? 20 : 16);
    lv_obj_set_height(row, content_height < 48 ? 48 : content_height);

    if (interactive) {
        if (callback != NULL) lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
        if (group != NULL) lv_group_add_obj(group, row);
    }
    return row;
}

lv_obj_t *ui_kit_create_compact_hold_action(lv_obj_t *parent, lv_group_t *group,
                                            lv_coord_t width, lv_coord_t height,
                                            const char *label_text, ui_tone_t tone,
                                            lv_event_cb_t callback)
{
    if (parent == NULL) return NULL;
    lv_obj_t *action = lv_btn_create(parent);
    if (action == NULL) return NULL;
    lv_obj_set_size(action, width, height);
    lv_obj_add_style(action, &row_style, LV_PART_MAIN);
    lv_obj_add_style(action, &row_focused_style, LV_PART_MAIN | LV_STATE_FOCUSED);
    if (group != NULL) {
        lv_obj_add_event_cb(action, touch_focus_row_event, LV_EVENT_PRESSED, group);
        lv_group_add_obj(group, action);
    }
    if (callback != NULL) lv_obj_add_event_cb(action, callback, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *label = lv_label_create(action);
    if (label == NULL) {
        lv_obj_del(action);
        return NULL;
    }
    lv_label_set_text_fmt(label, "%s " LV_SYMBOL_PLAY, label_text == NULL ? "" : label_text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, ui_kit_tone_color(tone), LV_PART_MAIN);
    lv_obj_center(label);
    return action;
}

void ui_kit_set_row_text(lv_obj_t *row, const char *title, const char *value)
{
    if (row == NULL) return;
    lv_obj_t *title_label = lv_obj_get_child(row, 0);
    lv_obj_t *value_label = lv_obj_get_child(row, 1);
    if (title_label != NULL) lv_label_set_text(title_label, title == NULL ? "" : title);
    if (value_label != NULL) lv_label_set_text(value_label, value == NULL ? "" : value);
    layout_item_text(row, title_label, value_label);
}

void ui_kit_set_row_subtitle(lv_obj_t *row, const char *subtitle)
{
    if (row == NULL) return;
    lv_obj_t *title_label = lv_obj_get_child(row, 0);
    lv_obj_t *value_label = lv_obj_get_child(row, 1);
    lv_obj_t *subtitle_label = lv_obj_get_child(row, 2);
    const bool visible = subtitle != NULL && subtitle[0] != '\0';
    lv_obj_set_height(row, visible ? 66 : 48);
    if (title_label != NULL) lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, visible ? -10 : 0);
    if (value_label != NULL) lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, 0, visible ? -10 : 0);
    if (subtitle_label != NULL) lv_label_set_text(subtitle_label, visible ? subtitle : "");
}

void ui_kit_set_row_tone(lv_obj_t *row, ui_tone_t tone)
{
    if (row == NULL) return;
    lv_obj_t *value_label = lv_obj_get_child(row, 1);
    if (value_label != NULL) {
        lv_obj_set_style_text_color(value_label, ui_kit_tone_color(tone), LV_PART_MAIN);
    }
}
