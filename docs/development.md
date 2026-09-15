# Firmware development

This guide is for contributors who need to change and compile the firmware.
Ordinary users should use the [browser installer](firmware.md) instead.

The project requires ESP-IDF **v5.4.1** and targets the ESP32-S3. The official
ESP-IDF installation guides explain how to install its compiler and tools for
[macOS/Linux](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/linux-macos-setup.html)
and
[Windows](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/windows-setup.html).

With ESP-IDF activated in your shell, build from the repository root:

```bash
idf.py -C firmware build
```

For the repository-managed local ESP-IDF installation, use:

```bash
scripts/build.sh
scripts/flash-monitor.sh /dev/ttyUSB0
```

The release workflow creates two different assets:

- `klipper-pendant-onx3248g035-install.bin` — full image for the initial USB install;
- `klipper-pendant-onx3248g035.bin` — application-only image for OTA updates.

Do not use the OTA image alone for the first installation.
