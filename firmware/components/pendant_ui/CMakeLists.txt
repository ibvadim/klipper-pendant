idf_component_register(
    SRCS "pendant_ui.c" "ui_kit.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_lvgl_port pendant_input board settings wifi_manager printer moonraker ota_update
)
