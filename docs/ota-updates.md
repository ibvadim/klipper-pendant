# OTA firmware releases

The firmware uses two application slots. The release currently running stays
untouched while the next image is downloaded into the inactive slot. It is
selected only after its SHA-256 matches the release manifest and ESP-IDF has
validated the image; bootloader rollback restores the old slot if the new app
does not finish starting.

The Firmware screen obtains recent version tags from GitHub and validates each
release's manifest before showing it. A tag that is still being prepared, or
is missing its manifest, is ignored rather than breaking update checks for
every pendant. This avoids downloading generated release notes just to list
installable firmware.

Opening Firmware pauses the Moonraker WebSocket and releases its transport
buffers. Release-list data is allocated in PSRAM only during a check; the
temporary index is freed immediately, and the selected list is discarded when
leaving Firmware. This leaves internal RAM and Wi-Fi available for TLS and the
firmware download.

## First installation

The former factory layout has only one application partition. For the first
USB installation, use the browser installer or the complete
`klipper-pendant-onx3248g035-install.bin` from a GitHub Release; it writes the new
bootloader and partition table. Do not use an OTA image for the first install.

## Publishing

After committing the release contents, run `make release VERSION=0.2.1`.
It pushes `main`, creates the corresponding annotated tag, and pushes that tag.
The release workflow builds the OTA application image
`klipper-pendant-onx3248g035.bin` and a complete first-install image
`klipper-pendant-onx3248g035-install.bin`, calculates a SHA-256 checksum for each, and
uploads both images plus `manifest.json` to a draft GitHub Release. It makes
the release public only after all assets are present. The device accepts only
HTTPS URLs under this repository's GitHub Release download path and only
manifests for the `onx3248g035` board.

On the pendant, open **Settings → Firmware**, check for updates, then choose a
listed release and hold **Install**. Keep power connected until it reboots;
all pendant input is locked while the firmware is downloading and verifying.
