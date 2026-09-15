idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        pendant_input
        pendant_ui
        board
        nvs_flash
        settings
        wifi_manager
        printer
        moonraker
        ota_update
        app_update
)
