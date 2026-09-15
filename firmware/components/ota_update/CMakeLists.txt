idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_client freertos json mbedtls moonraker
)
