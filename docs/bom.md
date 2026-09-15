# Bill of materials

This list covers one pendant. Product links are examples, not endorsements;
equivalent parts are fine when their dimensions and electrical characteristics
match. Check the battery connector polarity before connecting it.

| Part | Qty. | Specification / notes | Example source |
| --- | ---: | --- | --- |
| Touchscreen controller | 1 | OpenNextion ONX3248G035, ESP32-S3R8, 3.5-inch | [OpenNextion documentation](https://github.com/OpenNextion/OpenNextion-SKU-ONX3248G035), [purchase](https://ali.click/qc2ol14) |
| IO adapter | 1 | Nextion IO Adapter V2 | [purchase](https://ali.click/xb2ol1d) |
| Rotary encoder | 1 | EC11, with push switch | [purchase](https://ali.click/hf2ol19) |
| Tactile buttons | 2 | 12 × 12 × 7.3 mm; used for Back and E-STOP | [purchase](https://ali.click/vq2ol1o) |
| Li-ion battery | 1 | 104050, 3.7 V, about 2500 mAh | [purchase](https://ali.click/dc3ol16) or a local supplier |
| Battery connector | 1 | JST MX 1.25, 2-pin; **verify polarity** | [purchase](https://ali.click/7i3ol1j) |
| Screws | 9 | ISO 7380 M3 × 6 mm (or equivalent M3 × 6 mm) | [purchase](https://ali.click/kn3ol15) |
| Magnets | 4 | 8 × 3 mm | [purchase](https://ali.click/d14ol1f) |
| Heat-set inserts | 4 | M3, 3–4 mm long | [purchase](https://ali.click/8w3ol1b) |
| Hook-up wire | as needed | Thin, flexible wire for controls and IO adapter | local supplier |

## Tools and consumables

- 3D printer and filament
- Soldering iron, solder, and flux

## Printing

The current enclosure source is [`../3D/pendant v1.stp`](../3D/pendant%20v1.stp).
Export the individual bodies for your slicer. The intended print set is:

- top panel — 1
- bottom panel — 1
- buttons — 2
- power-toggle fork — 1
- power-toggle switch — 1
- knob - 1

Starting settings used for the prototype: PLA, 2 perimeters, 3 top and bottom
layers, and 15% infill. A well-calibrated printer is important: the enclosure
uses tight fits for the screen, buttons, and magnets.

## Before ordering

The assembly guide will document the required component orientations and wire
lengths. Until then, do not substitute the encoder, buttons, battery, magnets,
or inserts solely by appearance: their size affects enclosure fit.
