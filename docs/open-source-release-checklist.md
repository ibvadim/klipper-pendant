# Open-source release checklist

Use this checklist before announcing the project. It separates material needed
to understand the project from material needed to reproduce it safely.

## Must be complete

- [x] Add a repository-root MIT licence covering the firmware, enclosure
  design, and documentation.
- [ ] Complete the [assembly guide](assembly.md) with a wiring diagram,
  photographs, component orientations, and a final functional test.
- [ ] Confirm that every BOM link, quantity, part dimension, and battery
  polarity warning is accurate.
- [ ] Provide printable exports (STL/3MF) for each enclosure part, or explain
  the supported STEP-to-slicer workflow.
- [ ] Publish a tested firmware release with the OTA image, full USB-install
  image, checksums, and clear supported hardware revision.
- [ ] Enable GitHub Pages and test the full newcomer path on a clean computer:
  open the browser installer, flash, configure, and connect to a printer.
- [ ] Remove device backups, Wi-Fi credentials, serial numbers, and local files
  before pushing.

## Strongly recommended

- [ ] Add one real photograph of the completed pendant and screenshots of the
  most important screens.
- [ ] Tag the enclosure and firmware together with a hardware revision (for
  example, `hardware-v1`).
- [ ] Add `CONTRIBUTING.md` explaining how to report bugs and submit hardware or
  firmware changes.
- [ ] Add issue templates for bug reports and hardware-build feedback.
- [ ] State which Klipper and Moonraker versions were tested.
- [ ] Describe the expected battery runtime, charging method, and whether the
  device may be used while charging.

## Licence scope

The repository is distributed under the [MIT License](../LICENSE). Third-party
components retain the licences listed in
[`firmware/THIRD_PARTY_NOTICES.md`](../firmware/THIRD_PARTY_NOTICES.md).
