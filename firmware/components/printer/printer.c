#include "printer.h"

#include <string.h>

#include "esp_check.h"
#include "settings.h"

static printer_source_t source;

static esp_err_t mock_get_state(printer_state_t *state, void *context)
{
    (void)context;
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    settings_printer_t config;
    ESP_RETURN_ON_ERROR(settings_get_printer(&config), "printer", "read printer configuration");
    if (!settings_printer_is_valid(&config) || !config.enabled) {
        strlcpy(state->name, "Printer", sizeof(state->name));
        state->connection = PRINTER_CONNECTION_UNCONFIGURED;
        return ESP_OK;
    }
    strlcpy(state->name, config.name, sizeof(state->name));
    /* This is deliberately a local mock until a read-only Moonraker source is added. */
    state->connection = PRINTER_CONNECTION_OFFLINE;
    return ESP_OK;
}

esp_err_t printer_init(void)
{
    printer_use_mock_source();
    return ESP_OK;
}

esp_err_t printer_get_state(printer_state_t *state)
{
    if (source.get_state == NULL) return ESP_ERR_INVALID_STATE;
    return source.get_state(state, source.context);
}

static void add_legacy_heater(printer_capabilities_t *capabilities, const char *id,
                              const char *label, int16_t current, int16_t target,
                              bool available)
{
    if (capabilities->heater_count >= PRINTER_MAX_HEATERS) return;
    printer_heater_capability_t *heater =
        &capabilities->heaters[capabilities->heater_count++];
    strlcpy(heater->id, id, sizeof(heater->id));
    strlcpy(heater->label, label, sizeof(heater->label));
    heater->current_deci_c = current;
    heater->target_deci_c = target;
    heater->min_deci_c = 0;
    heater->max_deci_c = strcmp(id, "heater_bed") == 0 ? 1200 : 3000;
    heater->available = available;
    heater->controllable = available;
}

esp_err_t printer_get_capabilities(printer_capabilities_t *capabilities)
{
    if (capabilities == NULL) return ESP_ERR_INVALID_ARG;
    if (source.get_capabilities != NULL) {
        return source.get_capabilities(capabilities, source.context);
    }

    printer_state_t state;
    ESP_RETURN_ON_ERROR(printer_get_state(&state), "printer", "read legacy state");
    memset(capabilities, 0, sizeof(*capabilities));
    if (state.connection == PRINTER_CONNECTION_UNCONFIGURED) return ESP_OK;

    const bool available = state.connection == PRINTER_CONNECTION_ONLINE;
    capabilities->speed_factor = true;
    capabilities->flow_factor = true;
    capabilities->files = true;
    capabilities->pause = true;
    capabilities->axes[0] = true;
    capabilities->axes[1] = true;
    capabilities->axes[2] = true;
    add_legacy_heater(capabilities, "extruder", "Hotend",
                      state.hotend_current_deci_c, state.hotend_target_deci_c, available);
    add_legacy_heater(capabilities, "heater_bed", "Bed",
                      state.bed_current_deci_c, state.bed_target_deci_c, available);

    printer_fan_capability_t *fan = &capabilities->fans[capabilities->fan_count++];
    strlcpy(fan->id, "fan", sizeof(fan->id));
    strlcpy(fan->label, "Part fan", sizeof(fan->label));
    fan->speed_percent = state.fan_speed_percent;
    fan->available = available;
    fan->controllable = available;
    return ESP_OK;
}

esp_err_t printer_set_source(const printer_source_t *new_source)
{
    if (new_source == NULL || new_source->get_state == NULL) return ESP_ERR_INVALID_ARG;
    source = *new_source;
    return ESP_OK;
}

void printer_use_mock_source(void)
{
    source = (printer_source_t) { .get_state = mock_get_state, .context = NULL };
}
