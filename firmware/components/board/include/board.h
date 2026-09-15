#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { BOARD_BATTERY_DISCHARGING, BOARD_BATTERY_CHARGING, BOARD_BATTERY_UNAVAILABLE } board_battery_charge_state_t;
typedef struct {
    uint16_t voltage_mv;
    uint8_t percent;
    board_battery_charge_state_t charge_state;
    bool valid;
    bool present;
} board_battery_status_t;
esp_err_t board_init(void);
void board_battery_get_status(board_battery_status_t *status);
esp_err_t board_set_backlight(uint8_t percent);
uint8_t board_get_backlight(void);
