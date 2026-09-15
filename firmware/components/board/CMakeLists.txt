idf_component_register(SRCS "board.c" INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_spi esp_driver_ledc esp_adc esp_lcd
    esp_lvgl_port esp_lcd_touch esp_lcd_exio_pcf8574 esp_lcd_st7796u
    esp_lcd_touch_cst826 heap)
