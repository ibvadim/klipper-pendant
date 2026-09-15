# Install firmware

## Recommended: install in the browser

No build tools, Git, Python, or ESP-IDF are needed.

1. Use a desktop computer with the latest Chrome or Edge browser. Safari, Firefox,
   and mobile browsers cannot use this installer.
2. Connect the OpenNextion ONX3248G035 with a USB **data** cable. A charging-only
   cable will not work.
3. Open [the web installer](https://ibvadim.github.io/klipper-pendant/).
4. Click **Connect and install**, select the new USB device, then confirm the
   installation. Do not unplug it while writing is in progress.
5. When the display restarts, open **Settings** and enter Wi-Fi and Moonraker
   details.

The installer uses the latest published GitHub Release. It is available after
the first release has been published.

> **First installation replaces the factory firmware.** 

## If the browser installer is unavailable

Download `klipper-pendant-onx3248g035-install.bin` from the latest
[GitHub Release](https://github.com/ibvadim/klipper-pendant/releases/latest). It is a
complete first-install image, not the smaller OTA update file.

Install the small flashing utility, then write the downloaded file. Replace
`PORT` and `PATH/TO/FILE` with your USB port and downloaded file path:

```bash
python3 -m pip install esptool
python3 -m esptool --chip esp32s3 --port PORT write-flash 0x0 PATH/TO/klipper-pendant-onx3248g035-install.bin
```

Typical port names are `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` or
`/dev/ttyUSB0` on Linux, and `COM3` on Windows. On Windows, use `python`
instead of `python3`.

## Updating later

After the first USB installation, update from **Settings → Firmware** on the
pendant. This downloads the smaller OTA image and preserves the device setup.

## Developing the firmware

Only contributors who need to change firmware should build it from source. See
[the development guide](development.md).
