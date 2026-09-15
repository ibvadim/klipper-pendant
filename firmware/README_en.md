# ESP32-S3 Touch Test Example

## Overview

This project is a touch function test example for the ONX3248G035 development board (ESP32-S3). It communicates with a CST826 touch controller via the I2C interface and drives an ST7796U LCD screen using the SPI interface. The example uses the LVGL graphics library to display the touch trace on the screen in real-time.

When the user touches the screen, the program reads the touch coordinates and draws a red 4x4 pixel square at the corresponding position, providing a visual verification of the touch functionality.

## Purchase Links

- Product: [Open Nextion 3.5" Genius Series ESP32-S3 LCD Touchscreen Development Board](https://itead.cc/product/open-nextion-3-5-genius-series-esp32-s3-lcd-touchscreen-development-board/)

## Features

- **Touch Detection**: Real-time touch input detection via the CST826 touch controller.
- **Screen Display**: Uses an ST7796U LCD screen to display the graphical interface.
- **LVGL Integration**: Integrates the LVGL library for UI rendering and touch event handling.
- **Real-time Feedback**: Draws red squares on the screen to show the touch position in real-time.
- **Backlight Control**: Supports adjusting the LCD backlight brightness via PWM.

## Hardware Requirements

- **MCU**: ESP32-S3 Development Board
- **LCD Screen**: 3.5-inch ST7796U SPI TFT LCD (Resolution: 320x480)
- **Touch Controller**: CST826 I2C Capacitive Touch Controller
- **IO Expander**: PCF8574 I2C IO Expander (used for LCD reset)

## Pin Connections

| Function | ESP32-S3 Pin | Description |
| :--- | :--- | :--- |
| **I2C** | | |
| I2C SCL | GPIO_NUM_7 | I2C Clock Line |
| I2C SDA | GPIO_NUM_8 | I2C Data Line |
| **SPI** | | |
| SPI SCLK | GPIO_NUM_5 | SPI Clock Line |
| SPI MOSI | GPIO_NUM_1 | SPI Master Out Slave In Data Line |
| LCD DC | GPIO_NUM_3 | LCD Data/Command Select |
| LCD CS | GPIO_NUM_2 | LCD Chip Select |
| **Other** | | |
| LCD BL | GPIO_NUM_6 | LCD Backlight Control (PWM) |
| LCD RST | - | Controlled via PCF8574 |

## Compilation and Flashing

### Environment Setup

Please ensure you have installed and configured the [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) development environment.

### Compilation Steps

1.  **Set Target Chip**:
    ```bash
    idf.py set-target esp32s3
    ```

2.  **Configure Project** (Optional):
    Run `idf.py menuconfig`, and under the `Board Configuration` menu, you can modify pin definitions, LCD resolution, and other settings based on your hardware configuration.

3.  **Build the Project**:
    ```bash
    idf.py build
    ```

### Flashing Steps

1.  **Connect the Board**: Connect the ESP32-S3 development board to your computer via USB.

2.  **Flash the Firmware**:
    ```bash
    idf.py -p <YOUR_SERIAL_PORT> flash
    ```
    Replace `<YOUR_SERIAL_PORT>` with the serial port corresponding to your ESP32-S3 board (e.g., `COM3` or `/dev/ttyUSB0`).

3.  **Monitor Output**:
    ```bash
    idf.py -p <YOUR_SERIAL_PORT> monitor
    ```
    Use the serial monitor to view program logs and debugging information.