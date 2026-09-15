# Klipper Pendant

An open hardware, handheld touchscreen controller for a Klipper 3D printer.
It connects to Moonraker over Wi-Fi and gives the printer a dedicated physical
interface for everyday operation and servicing.

![Rendered enclosure](docs/main.png)

> **Project status: work in progress.** The firmware and enclosure design are
> published here; the illustrated assembly guide is still being prepared.

## What can it do?

- Monitor the current print, temperatures, progress, and printer state.
- Start, pause, resume, exclude objects and cancel prints.
- Move and home axes, adjust temperatures, fan, speed, and flow.
- Run Klipper macros, use the G-code console, and view job history.
- Manage more than one printer from the pendant.
- Use a physical rotary encoder, Back button, and E-STOP button in addition
  to the 3.5-inch touch display.

The pendant is designed for printers running **Klipper with Moonraker**. It is
not a general-purpose USB controller and does not replace the printer's normal
safety systems.

## Build one

Follow this path from parts to a working pendant:

1. Read the [bill of materials](docs/bom.md) and order the required parts.
2. Print the enclosure from the files in [3D](3D/).
3. Follow the [assembly guide](docs/assembly.md). It is a draft until the
   wiring diagram and build photos are added.
4. [Install the latest firmware](docs/firmware.md) over USB; no build tools are
   required.
5. On the pendant, enter Wi-Fi and Moonraker details in **Settings**, then
   select the printer.

## Safety

The E-STOP button sends Moonraker's `emergency_stop` request; it is **not** a
hard-wired power disconnect. Keep the printer's independent, physical
emergency-stop and electrical protection in place. Disconnect the battery
before soldering, and never apply 5 V to the OpenNextion GPIO inputs.

## Hardware at a glance

| Part | Choice used by this project |
| --- | --- |
| Display/controller | OpenNextion ONX3248G035 (ESP32-S3R8, 16 MiB flash, 8 MiB PSRAM) |
| IO breakout | Nextion IO Adapter V2 |
| Controls | EC11 rotary encoder, Back button, E-STOP button |
| Power | 3.7 V 104050 Li-ion battery |
| Enclosure | 3D-printed two-piece case with magnets and M3 heat-set inserts |

The exact pin assignments and the 3.3 V wiring rule are in the [assembly
guide](docs/assembly.md).

## Repository layout

| Path | Contents |
| --- | --- |
| [`3D/`](3D/) | Printable enclosure source (STEP) and preview render |
| [`docs/`](docs/) | BOM, assembly, installation, and release documentation |
| [`firmware/`](firmware/) | ESP-IDF firmware for the ESP32-S3 |
| [`scripts/`](scripts/) | Convenience scripts for local development and flashing |

## Contributing and license

Issues and pull requests are welcome. This project is licensed under the
[MIT License](LICENSE), including its firmware and enclosure design files.

For maintainers, [OTA release notes](docs/ota-updates.md) and
[third-party notices](firmware/THIRD_PARTY_NOTICES.md) remain available.
