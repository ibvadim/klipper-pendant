idf_component_register(
    SRCS "moonraker.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp_event freertos json settings wifi_manager printer
)
