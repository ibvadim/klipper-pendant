idf_component_register(
    SRCS "pendant_input.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio freertos board
)
