#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "pendant_input.h"
#include "pendant_ui.h"
#include "settings.h"
#include "wifi_manager.h"
#include "printer.h"
#include "moonraker.h"
#include "esp_ota_ops.h"

static void input_event_cb(const pendant_input_event_t *event, void *context)
{
    (void)context;
    pendant_ui_handle_input(event);
}

/* The stock hook only reports that a stack guard was hit.  Name the task so a
 * field log remains actionable even after its exception backtrace is corrupt. */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    ESP_EARLY_LOGE("stack", "Stack overflow in task: %s", task_name == NULL ? "<unknown>" : task_name);
    abort();
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(printer_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(moonraker_init());
    ESP_ERROR_CHECK(pendant_ui_init());
    ESP_ERROR_CHECK(pendant_input_init(input_event_cb, NULL));
    ESP_ERROR_CHECK(wifi_manager_start());
    /* Keep the previous slot bootable until the complete UI has started. */
    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
}
