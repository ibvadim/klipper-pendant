
#pragma once

/**
 * @file exio_pcf8574.h
 * @brief Header file for PCF8574 I2C expander IO control (ESP-IDF implementation)
 * 
 * This header provides macros, enumerations, and function prototypes for controlling
 * the PCF8574 8-bit I/O expander via I2C. It supports pin read/write operations for
 * various peripheral control pins (e.g., LCD reset, camera power down, touch interrupt).
 */

#include "esp_check.h"       
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def PCF8574_IIC_SPEED
 * @brief I2C communication speed for PCF8574 (100 KHz, standard mode)
 */
#define     PCF8574_IIC_SPEED       (100000) 

/**
 * @def PCF8574_ADDR
 * @brief Default I2C slave address of the PCF8574 expander (0x38)
 */
#define     PCF8574_ADDR            (0x38)

/**
 * @def PCF8574_RW_MAX_TIMEOUT
 * @brief Maximum timeout for I2C read/write operations (500 milliseconds)
 */
#define     PCF8574_RW_MAX_TIMEOUT  (500)

/**
 * @enum exio_pcf8574_pin
 * @brief Enumeration of PCF8574 expander pins mapped to specific peripheral functions
 */
typedef enum  
{
    EXIO_PCF8574_PIN_I2S_CTRL = 1,  /**< Pin for I2S peripheral control */
    EXIO_PCF8574_PIN_RTC_INT,       /**< Pin for RTC interrupt signal */
    EXIO_PCF8574_PIN_CHG_N,         /**< Pin for battery charging status (active low) */
    EXIO_PCF8574_PIN_CAM_PWDN,      /**< Pin for camera power down control */
    EXIO_PCF8574_PIN_TP_INT,        /**< Pin for touch panel interrupt signal */
    EXIO_PCF8574_PIN_LCD_RST,       /**< Pin for LCD reset control */
    EXIO_PCF8574_PIN_SDCS           /**< Pin for SD card chip select control */
} exio_pcf8574_pin;

/**
 * @enum exio_pcf8574_level
 * @brief Enumeration of pin logic levels (low/high)
 */
typedef enum  
{
    EXIO_PCF8574_LEVEL_L = 0,  /**< Logic low level (0V) */
    EXIO_PCF8574_LEVEL_H,      /**< Logic high level (VCC) */
} exio_pcf8574_level;

/**
 * @brief Initialize the PCF8574 I2C expander
 * 
 * Configures the I2C master bus, establishes communication with the PCF8574,
 * and initializes necessary resources for pin operations.
 * 
 * @return esp_err_t ESP_OK if initialization succeeds; error code otherwise
 */
esp_err_t exio_pcf8574_init(void);

/**
 * @brief Read the logic level of a specific PCF8574 pin
 * 
 * @param pin Target pin to read (from exio_pcf8574_pin enumeration)
 * @param level Pointer to store the read pin level (output parameter)
 * 
 * @return esp_err_t ESP_OK if read succeeds; error code otherwise
 */
esp_err_t exio_pcf8574_pin_read(exio_pcf8574_pin pin, exio_pcf8574_level *level);

/**
 * @brief Set the logic level of a specific PCF8574 pin
 * 
 * @param pin Target pin to write (from exio_pcf8574_pin enumeration)
 * @param level Desired logic level (EXIO_PCF8574_LEVEL_L or EXIO_PCF8574_LEVEL_H)
 * 
 * @return esp_err_t ESP_OK if write succeeds; error code otherwise
 */
esp_err_t exio_pcf8574_pin_write(exio_pcf8574_pin pin, exio_pcf8574_level level);

#ifdef __cplusplus
}
#endif