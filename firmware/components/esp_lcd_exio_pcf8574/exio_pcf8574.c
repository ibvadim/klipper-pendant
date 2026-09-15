
#include "exio_pcf8574.h"

/**
 * @brief Log tag for PCF8574 related debug messages
 */
const char *pcf8574_tag = "pcf8574";

/**
 * @brief Current I/O state of PCF8574 I/O expander
 * 
 * This variable is used to track the current state of all 8 I/O pins
 * on the PCF8574 I2C I/O expander chip in software.
 * 
 * - Initialized to 0xFF (all bits high) because PCF8574 ports are
 *   configured as inputs with pull-up resistors enabled by default
 * - Marked as volatile since it may be modified by interrupt service
 *   routines or in multi-threaded environments
 * - Each bit represents the state of one I/O pin (1 = high, 0 = low)
 * - Bit 0 = P0, Bit 1 = P1, ..., Bit 7 = P7
 * 
 * In input mode, reading a high value (1) indicates the pin is floating
 * or pulled high, while low (0) indicates external pull-down.
 */
static volatile uint8_t pcf8574_now_io_state = 0xFF;

/**
 * @brief I2C master device handle for communicating with PCF8574
 */
static i2c_master_dev_handle_t pcf8574_i2c_handle = NULL;

/**
 * @brief Read the logic level of a specific PCF8574 pin
 * 
 * Reads the 8-bit IO state from the PCF8574 via I2C, then extracts the logic level of the target pin
 * using a bitmask. The result is stored in the provided `level` pointer.
 * 
 * @param pin Target pin to read (from exio_pcf8574_pin enumeration)
 * @param level Pointer to store the read logic level (EXIO_PCF8574_LEVEL_L/H)
 * @return esp_err_t ESP_OK if read succeeds; error code (e.g., I2C communication failure) otherwise
 */
esp_err_t exio_pcf8574_pin_read(exio_pcf8574_pin pin, exio_pcf8574_level *level)
{
    esp_err_t ret;
    uint8_t bit_mask = (0x01 << (pin - 1));  // Bitmask for the target pin (pins are 1-indexed in enum)
    uint8_t read_io_state = 0;               // Buffer to store the 8-bit IO state read from PCF8574

    // Read 1 byte of data (8-bit IO state) from PCF8574 via I2C
    ret = i2c_master_receive(pcf8574_i2c_handle, &read_io_state, 1, PCF8574_RW_MAX_TIMEOUT);

    if (ret == ESP_OK)
    {
        // Check if the target pin's bit is set (high level) or cleared (low level)
        if (read_io_state & bit_mask)
        {
            *level = EXIO_PCF8574_LEVEL_H;
        }
        else
        {
            *level = EXIO_PCF8574_LEVEL_L;
        }
    }

    return ret;
}

/**
 * @brief Set the logic level of a specific PCF8574 pin
 * 
 * Updates the cached IO state (pcf8574_now_io_state) for the target pin, then transmits the
 * updated 8-bit state to the PCF8574 via I2C. This avoids reading the current state from the expander.
 * 
 * @param pin Target pin to write (from exio_pcf8574_pin enumeration)
 * @param level Desired logic level (EXIO_PCF8574_LEVEL_L or EXIO_PCF8574_LEVEL_H)
 * @return esp_err_t ESP_OK if write succeeds; error code (e.g., I2C communication failure) otherwise
 */
esp_err_t exio_pcf8574_pin_write(exio_pcf8574_pin pin, exio_pcf8574_level level)
{
    esp_err_t ret = ESP_FAIL;
    uint8_t bit_mask = 0x01;  // Base bitmask for pin manipulation
    uint8_t cmd[1];            // Buffer to hold the 8-bit IO state to transmit

    // Update the cached IO state: set or clear the target pin's bit
    if (level == EXIO_PCF8574_LEVEL_H)
    {
        pcf8574_now_io_state |= (bit_mask << (pin - 1));  // Set the target pin's bit (high level)
    }
    else
    {
        pcf8574_now_io_state &= (~(bit_mask << (pin - 1)));  // Clear the target pin's bit (low level)
    }

    cmd[0] = pcf8574_now_io_state;  // Load the updated IO state into the transmit buffer

    // Transmit the 1-byte IO state to PCF8574 via I2C
    ret = i2c_master_transmit(pcf8574_i2c_handle, cmd, 1, PCF8574_RW_MAX_TIMEOUT);

    return ret;
}

/**
 * @brief Initialize the PCF8574 I2C expander
 * 
 * Retrieves the handle for I2C master bus 0, configures the PCF8574's I2C device parameters,
 * and adds the PCF8574 to the I2C bus. Initializes the I2C device handle for subsequent operations.
 * 
 * @return esp_err_t ESP_OK if initialization succeeds; ESP_FAIL if I2C handle creation fails
 */
esp_err_t exio_pcf8574_init(void)
{
    i2c_master_bus_handle_t i2c_bus_0_handle;  // Handle for I2C master bus 0

    // Configuration structure for PCF8574 I2C device parameters
    i2c_device_config_t pcf8574_i2c_dev_conf = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,  // 7-bit I2C slave address
        .scl_speed_hz    = PCF8574_IIC_SPEED,   // I2C communication speed (100 KHz)
        .device_address  = PCF8574_ADDR,        // PCF8574 slave address (0x38)
    };

    // Get the handle for I2C master bus 0 (must be initialized beforehand)
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &i2c_bus_0_handle));

    // Add PCF8574 device to the I2C bus if the bus handle is valid
    if (i2c_bus_0_handle != NULL)
    {
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_0_handle, &pcf8574_i2c_dev_conf, &pcf8574_i2c_handle));
    }

    // Check if the PCF8574 I2C device handle was created successfully
    if (pcf8574_i2c_handle == NULL)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}
