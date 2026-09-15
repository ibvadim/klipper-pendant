#pragma once

#include "esp_err.h"
#include "pendant_input.h"

/** Creates the offline UI prototype after LVGL and the display are ready. */
esp_err_t pendant_ui_init(void);

/** Routes a debounced physical-control event into the same LVGL UI as touch. */
void pendant_ui_handle_input(const pendant_input_event_t *event);
