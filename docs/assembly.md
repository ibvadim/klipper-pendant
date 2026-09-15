# Assembly guide

> **Draft.** This page establishes the build order and verified electrical
> constraints. Add annotated photographs, the wiring diagram, and the final
> mechanical orientations before presenting it as a complete beginner guide.

## Electrical connections

All controls share the OpenNextion/IO-adapter ground. Inputs are active low:
each switch connects its signal to GND when pressed.

| Control | GPIO | Connection |
| --- | ---: | --- |
| Encoder CLK / S1 | GPIO 12 | Encoder CLK output |
| Encoder DT / S2 | GPIO 13 | Encoder DT output |
| Encoder push / KEY | GPIO 17 | Encoder switch to GND |
| Back button | GPIO 18 | Switch to GND |
| E-STOP button | GPIO 21 | Switch to GND |
| Battery measurement | GPIO 4 | Board battery-sense connection |

Power an encoder **module** from 3.3 V so that its onboard pull-ups produce
3.3 V logic. Do not connect its VCC or control signals to 5 V. Confirm the
battery connector polarity with a multimeter before plugging it into the
board.

## Planned build sequence

1. Print enclosure parts listed in the [BOM](bom.md), remove supports,
   and test-fit the screen, buttons, encoder, inserts, and magnets.
2. Install the four M3 heat-set inserts in back panel. Keep them square with the plastic.
3. Install magnets in back panel. Record the chosen orientation in the future photo guide.
4. Wire the encoder and two buttons to the IO adapter according to the table
   above; inspect for shorts.
5. Test the controls before final enclosure assembly, then install the battery
   and secure the case with the nine M3 × 6 mm screws.
6. Flash the firmware using the [firmware guide](firmware.md), configure Wi-Fi
   and Moonraker, and verify every control before using it near a printer.

## Required additions before release

- A labelled wiring diagram showing the IO Adapter V2 terminals and battery
  connection
- Photos for each assembly stage, including magnet polarity and cable routing
- The exact screen orientation, heat-set-insert locations, and enclosure screw
  map
- A power-on and functional test checklist

Do not treat the device's software E-STOP request as electrical isolation.
Keep a separately wired, physical emergency stop for the printer.
