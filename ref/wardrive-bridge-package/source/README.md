# WardriveESP

ESP32 firmware that turns a small board into a Wi-Fi and Bluetooth scanner for the Wardrive Go app on iOS and
Android. The board links to the app over Bluetooth and streams everything it hears. No pairing, no setup.
Several boards can link at once and split the channels between them.

## Supported chips

- ESP32-C3
- ESP32-C5 (dual-band, 2.4 + 5 GHz)
- ESP32-C6
- ESP32-S3

## Flash

- Web flasher: https://rocketgod.dev/WardriveESP/
- Android: plug the board into your phone and use the USB flasher in Wardrive Go.
- esptool: grab `WardriveESP-<chip>-merged.bin` from
  [Releases](https://github.com/RocketGod-git/WardriveESP/releases) and write it at `0x0`.

```sh
esptool --chip esp32c6 -p <PORT> erase_flash
esptool --chip esp32c6 -p <PORT> -b 460800 write_flash 0x0 WardriveESP-esp32c6-merged.bin
```

## Updates

After the first flash, updates install over Bluetooth from the app.

## Build

ESP-IDF 5.5.

```sh
idf.py set-target esp32c6
idf.py build flash
```

## Protocol

BLE and USB protocol: [PROTOCOL.md](PROTOCOL.md)

## For flasher tools

- Release assets, per chip (`esp32c3`, `esp32c5`, `esp32c6`, `esp32s3`):
  - `WardriveESP-<chip>-merged.bin`: full image, flash at `0x0`
  - `WardriveESP-<chip>-app.bin`: app image only, for OTA
- Flash size: 4 MB.
- Latest build: `https://github.com/RocketGod-git/WardriveESP/releases/latest/download/WardriveESP-<chip>-merged.bin`
- Latest version: https://rocketgod.dev/WardriveESP/version.json (`latest`)

## License

MIT. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
