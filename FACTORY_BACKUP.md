# ONX3248G035 factory backup

The original 16 MiB flash was read before any custom firmware was written.

```text
File: artifacts/onx3248g035-factory.bin
Size: 16777216 bytes
SHA-256: 731a2dc143af7d6773ced53ea5a4774007946e10715c2e2f651832e61b9f6a44
```

Verify before restoring:

```bash
sha256sum -c artifacts/onx3248g035-factory.bin.sha256
```

Restore the complete image:

```bash
uv run --with esptool esptool --chip esp32s3 --port /dev/ttyUSB0 \
  --baud 460800 write-flash 0x0 artifacts/onx3248g035-factory.bin
```

This replaces the complete flash contents of the connected ESP32-S3. Check the
port and checksum before running it.

