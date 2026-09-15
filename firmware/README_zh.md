# ESP32-S3 触摸测试示例

## 概述

本示例旨在演示如何将触摸功能集成到 ONX3248G035开发板(ESP32-S3)中。它通过I2C接口与CST826触摸控制器通信，并利用SPI接口驱动ST7796U LCD屏幕。示例使用LVGL图形库在屏幕上实时显示触摸轨迹。

当用户触摸屏幕时，程序会读取触摸坐标并在相应位置绘制一个红色的4x4像素方块，从而直观地验证触摸功能的正确性。

## 购买链接

- 产品购买：[Open Nextion 3.5" Genius Series ESP32-S3 LCD Touchscreen Development Board](https://itead.cc/product/open-nextion-3-5-genius-series-esp32-s3-lcd-touchscreen-development-board/)

## 功能

- **触摸检测**: 通过CST826触摸控制器实时检测触摸输入。
- **屏幕显示**: 使用ST7796U LCD屏幕显示图形界面。
- **LVGL集成**: 集成LVGL库，用于UI渲染和触摸事件处理。
- **实时反馈**: 在屏幕上绘制红色方块以实时显示触摸位置。
- **背光控制**: 支持通过PWM调节LCD背光亮度。

## 硬件要求

- **主控**: ESP32-S3开发板
- **LCD屏幕**: 3.5寸 ST7796U SPI接口TFT LCD (分辨率: 320x480)
- **触摸控制器**: CST826 I2C接口电容触摸控制器
- **IO扩展器**: PCF8574 I2C IO扩展器 (用于LCD复位)

## 引脚连接

| 功能 | ESP32-S3 引脚 | 说明 |
| :--- | :--- | :--- |
| **I2C** | | |
| I2C SCL | GPIO_NUM_7 | I2C时钟线 |
| I2C SDA | GPIO_NUM_8 | I2C数据线 |
| **SPI** | | |
| SPI SCLK | GPIO_NUM_5 | SPI时钟线 |
| SPI MOSI | GPIO_NUM_1 | SPI主出从入数据线 |
| LCD DC | GPIO_NUM_3 | LCD数据/命令选择 |
| LCD CS | GPIO_NUM_2 | LCD片选 |
| **其他** | | |
| LCD BL | GPIO_NUM_6 | LCD背光控制 (PWM) |
| LCD RST | - | 通过PCF8574控制 |

## 编译与烧录

### 环境配置

请确保您已安装并配置好[ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html)开发环境。

### 编译步骤

1. **设置目标芯片**:
   ```bash
   idf.py set-target esp32s3
   ```

2. **配置项目** (可选):
   运行 `idf.py menuconfig`，在 `Board Configuration` 菜单下，您可以根据您的硬件配置修改引脚定义、LCD分辨率等设置。

3. **编译项目**:
   ```bash
   idf.py build
   ```

### 烧录步骤

1. **连接开发板**: 将ESP32-S3开发板通过USB连接到您的计算机。

2. **烧录固件**:
   ```bash
   idf.py -p <您的串口号> flash
   ```
   将 `<您的串口号>` 替换为您的ESP32-S3开发板对应的串口号 (例如 `COM3` 或 `/dev/ttyUSB0`)。

3. **监视输出**:
   ```bash
   idf.py -p <您的串口号> monitor
   ```
   通过串口监视器查看程序运行日志和调试信息。