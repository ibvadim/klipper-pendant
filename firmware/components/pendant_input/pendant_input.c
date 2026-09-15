#include "pendant_input.h"
#include "board_pins.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DEBOUNCE_SAMPLES 4
#define ENCODER_QUEUE_LENGTH 32
#define ENCODER_HOLD_MS 800

typedef struct {
    gpio_num_t gpio;
    pendant_input_type_t type;
    bool stable_pressed;
    bool candidate_pressed;
    uint8_t candidate_samples;
    TickType_t pressed_at;
    bool hold_reported;
} button_state_t;

typedef struct {
    pendant_input_callback_t callback;
    void *callback_context;
    uint8_t encoder_state;
    int8_t encoder_accumulator;
    button_state_t buttons[3];
} input_context_t;

static input_context_t input_context;
static QueueHandle_t encoder_queue;

static void emit_event(pendant_input_type_t type, int delta, bool pressed)
{
    if (input_context.callback == NULL) {
        return;
    }

    const pendant_input_event_t event = {
        .type = type,
        .delta = delta,
        .pressed = pressed,
    };
    input_context.callback(&event, input_context.callback_context);
}

static void process_encoder_state(uint8_t current)
{
    static const int8_t transition[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };

    const uint8_t index = (input_context.encoder_state << 2) | current;
    input_context.encoder_state = current;
    input_context.encoder_accumulator += transition[index];

    if (input_context.encoder_accumulator >= 4) {
        input_context.encoder_accumulator = 0;
        emit_event(PENDANT_INPUT_ENCODER_ROTATE, 1, false);
    } else if (input_context.encoder_accumulator <= -4) {
        input_context.encoder_accumulator = 0;
        emit_event(PENDANT_INPUT_ENCODER_ROTATE, -1, false);
    }
}

/*
 * Capture every quadrature edge in the ISR.  The state-table decoder runs in
 * the task, where it can safely invoke the application callback.  This avoids
 * losing short contacts between polling iterations and rejects bounce through
 * invalid/reversed state transitions.
 */
static void IRAM_ATTR encoder_edge_isr(void *argument)
{
    (void)argument;
    const uint8_t state = ((uint8_t)gpio_get_level(PENDANT_ENCODER_CLK_GPIO) << 1) |
                          (uint8_t)gpio_get_level(PENDANT_ENCODER_DT_GPIO);
    BaseType_t higher_priority_task_woken = pdFALSE;

    xQueueSendFromISR(encoder_queue, &state, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void poll_button(button_state_t *button)
{
    const bool pressed = gpio_get_level(button->gpio) == 0;

    if (pressed != button->candidate_pressed) {
        button->candidate_pressed = pressed;
        button->candidate_samples = 1;
        return;
    }
    if (button->candidate_samples < DEBOUNCE_SAMPLES) {
        button->candidate_samples++;
    }
    if (button->candidate_samples == DEBOUNCE_SAMPLES &&
        button->stable_pressed != button->candidate_pressed) {
        button->stable_pressed = button->candidate_pressed;
        if (button->stable_pressed) {
            button->pressed_at = xTaskGetTickCount();
            button->hold_reported = false;
            /* Encoder needs its down edge for hold-progress feedback. The
             * auxiliary buttons deliberately wait for release, so their
             * short action cannot also run after a hold action. */
            if (button->type == PENDANT_INPUT_ENCODER_PRESS) {
                emit_event(PENDANT_INPUT_ENCODER_DOWN, 0, true);
            }
        } else if (button->type == PENDANT_INPUT_ENCODER_PRESS) {
            emit_event(PENDANT_INPUT_ENCODER_DOWN, 0, false);
            if (!button->hold_reported) emit_event(PENDANT_INPUT_ENCODER_PRESS, 0, true);
        } else if ((button->type == PENDANT_INPUT_LEFT || button->type == PENDANT_INPUT_RIGHT) &&
                   !button->hold_reported) {
            emit_event(button->type, 0, true);
        }
    }

    /* Holds are semantic input events, not UI gestures.  Report each exactly
     * once while it remains down; the UI decides whether its current scope
     * permits the action. */
    if (button->stable_pressed &&
        !button->hold_reported &&
        xTaskGetTickCount() - button->pressed_at >= pdMS_TO_TICKS(ENCODER_HOLD_MS)) {
        button->hold_reported = true;
        const pendant_input_type_t hold_type = button->type == PENDANT_INPUT_ENCODER_PRESS ?
            PENDANT_INPUT_ENCODER_HOLD : button->type == PENDANT_INPUT_LEFT ?
            PENDANT_INPUT_LEFT_HOLD : PENDANT_INPUT_RIGHT_HOLD;
        emit_event(hold_type, 0, true);
    }
}

static void input_task(void *argument)
{
    (void)argument;

    while (true) {
        uint8_t encoder_state;
        while (xQueueReceive(encoder_queue, &encoder_state, 0) == pdTRUE) {
            process_encoder_state(encoder_state);
        }
        for (size_t i = 0; i < sizeof(input_context.buttons) / sizeof(input_context.buttons[0]); i++) {
            poll_button(&input_context.buttons[i]);
        }
        // One tick is guaranteed to yield even when CONFIG_FREERTOS_HZ is 100.
        vTaskDelay(1);
    }
}

esp_err_t pendant_input_init(pendant_input_callback_t callback, void *context)
{
    const gpio_config_t encoder_config = {
        .pin_bit_mask = (1ULL << PENDANT_ENCODER_CLK_GPIO) |
                        (1ULL << PENDANT_ENCODER_DT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << PENDANT_ENCODER_SW_GPIO) |
                        (1ULL << PENDANT_BACK_GPIO) |
                        (1ULL << PENDANT_ESTOP_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t result = gpio_config(&encoder_config);
    if (result != ESP_OK) {
        return result;
    }
    result = gpio_config(&button_config);
    if (result != ESP_OK) {
        return result;
    }

    input_context.callback = callback;
    input_context.callback_context = context;
    input_context.encoder_state = (gpio_get_level(PENDANT_ENCODER_CLK_GPIO) << 1) |
                                  gpio_get_level(PENDANT_ENCODER_DT_GPIO);
    input_context.buttons[0] = (button_state_t){ .gpio = PENDANT_ENCODER_SW_GPIO, .type = PENDANT_INPUT_ENCODER_PRESS };
    input_context.buttons[1] = (button_state_t){ .gpio = PENDANT_BACK_GPIO, .type = PENDANT_INPUT_LEFT };
    input_context.buttons[2] = (button_state_t){ .gpio = PENDANT_ESTOP_GPIO, .type = PENDANT_INPUT_RIGHT };

    encoder_queue = xQueueCreate(ENCODER_QUEUE_LENGTH, sizeof(uint8_t));
    if (encoder_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * encoder_edge_isr uses gpio_get_level(), which is flash-resident in the
     * ESP-IDF GPIO driver.  Do not mark this service IRAM-safe: an encoder
     * edge can otherwise arrive while an NVS/flash operation has disabled the
     * cache and trigger a fatal cache-disabled access.  Missing a mechanical
     * encoder edge during the short flash-critical section is harmless.
     */
    result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = gpio_isr_handler_add(PENDANT_ENCODER_CLK_GPIO, encoder_edge_isr, NULL);
    if (result != ESP_OK) {
        return result;
    }
    result = gpio_isr_handler_add(PENDANT_ENCODER_DT_GPIO, encoder_edge_isr, NULL);
    if (result != ESP_OK) {
        return result;
    }

    /* Input callbacks only schedule UI work now, but keep enough headroom for
     * debounce, queue draining, and future non-UI input consumers. */
    if (xTaskCreate(input_task, "pendant_input", 8192, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
