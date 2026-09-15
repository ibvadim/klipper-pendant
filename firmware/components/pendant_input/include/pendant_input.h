#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    PENDANT_INPUT_ENCODER_ROTATE,
    /* Raw down/up edge used only for hold-progress feedback. */
    PENDANT_INPUT_ENCODER_DOWN,
    /* A completed short press, emitted only after release without a hold. */
    PENDANT_INPUT_ENCODER_PRESS,
    PENDANT_INPUT_ENCODER_HOLD,
    /* Physical auxiliary buttons. Their actions are assigned in Settings. */
    PENDANT_INPUT_LEFT,
    PENDANT_INPUT_LEFT_HOLD,
    PENDANT_INPUT_RIGHT,
    PENDANT_INPUT_RIGHT_HOLD,
    /* UI-normalized actions; never emitted by the input driver. */
    PENDANT_INPUT_BACK,
    PENDANT_INPUT_BACK_HOLD,
    PENDANT_INPUT_ESTOP,
    PENDANT_INPUT_ESTOP_HOLD,
} pendant_input_type_t;

typedef struct {
    pendant_input_type_t type;
    int delta;
    bool pressed;
} pendant_input_event_t;

typedef void (*pendant_input_callback_t)(const pendant_input_event_t *event, void *context);

esp_err_t pendant_input_init(pendant_input_callback_t callback, void *context);
