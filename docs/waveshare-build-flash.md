# Build and flash: Waveshare ESP32-S3 Touch AMOLED 2.06

The watch runs the stock WardriveESP ESP32-S3 image. There is no separate board build: the firmware recognizes the
watch at boot (`watch_probe()`, see `waveshare-port-analysis.md` §7 D1). The same image runs on every ESP32-S3 board.

## Prerequisites

- ESP-IDF **v5.5.2** at `~/esp/esp-idf`. This is the version verified here; upstream asks for "5.5".
- The watch on USB-C. It enumerates as `303a:1001` (USB-Serial-JTAG), usually `/dev/ttyACM0`, and your user needs
  access to that port (group `dialout`).

## Build (ESP32-S3, the watch)

One build directory per chip, so targets never overwrite each other's `sdkconfig`:

```sh
cd firmware
. ~/esp/esp-idf/export.sh
idf.py -B build-esp32s3 -D SDKCONFIG=build-esp32s3/sdkconfig set-target esp32s3   # once
idf.py -B build-esp32s3 -D SDKCONFIG=build-esp32s3/sdkconfig build
```

Expected:
- `wardrive_bridge.bin binary size 0x149b10 bytes … (31%) free`.
- The same three `-Wunused-function` warnings the untouched upstream source has: `display.c text_c`,
  `wardrive_bridge.c led_apply`, `board_is_awok`.

## Flash

**The first flash erases the factory demo.** To restore it, use Waveshare's image (16 MB, written at `0x0`):
`ref/ESP32-S3-Touch-AMOLED-2.06/firmware/ESP32-S3-Touch-AMOLED-2.06_FactoryOnly_20261016.bin`.

```sh
python -m esptool --chip esp32s3 -p /dev/ttyACM0 erase_flash      # first time only
idf.py -B build-esp32s3 -D SDKCONFIG=build-esp32s3/sdkconfig -p /dev/ttyACM0 flash
```

- **Layout:** the image uses upstream's 4 MB partition table (two 0x1E0000 OTA slots).
- **Flash-size warning:** the watch has 32 MB of flash, so every boot logs `Detected size(32768k) larger than the
  size in the binary image header(4096k)`. This is expected and harmless.
- **Erase and identity:** erasing wipes NVS. The watch's identity comes from its hardware, not NVS, so an erase
  doesn't lose it.

## Watching the log

Plain `idf.py monitor`, or any tool that drops DTR while RTS is high, **resets the watch** when it opens the port. A
reset drops the phone's link. To attach to a running watch without a reset, use:

```sh
idf.py -B build-esp32s3 -D SDKCONFIG=build-esp32s3/sdkconfig -p /dev/ttyACM0 monitor --no-reset
```

Checked on the watch: uptime went 495 s → 505 s across the run, with no boot banner.

A healthy boot logs:

```
WardriveESP: I2C on SDA 15 / SCL 14: 0x18 0x34 0x40 0x51 0x6b
WardriveESP: watch probe: AXP2101 IC type 4a, 4 of 4 board parts answered
WardriveESP: board: Waveshare AMOLED 2.06
WardriveESP: lcd: 410x502 CO5300 running
WardriveESP: GPS: GPIO 4 belongs to this board's own hardware, GPS off
WardriveESP: advertising as 'WardriveESP-XXXX'
```

The two `i2c.master: probe device timeout` errors during the I2C scan are expected. They come from `i2c_scan_log`
probing every address on the board bus. The watch identifies itself regardless.

## Other chips (regression builds)

`export.sh` only puts toolchains on `PATH` for the targets your IDF install registered. This machine registered
only `esp32`, so the RISC-V compiler for C3/C5/C6 has to be added by hand:

```sh
export PATH=$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin:$PATH
for t in esp32c3 esp32c5 esp32c6; do
  idf.py -B build-$t -D SDKCONFIG=build-$t/sdkconfig set-target $t && idf.py -B build-$t -D SDKCONFIG=build-$t/sdkconfig build
done
```

`esp32c5` does not build on IDF v5.5.2, and the untouched upstream source fails the same way:
`wifi_csi_config_t has no member named 'lltf_bit_mode'` (`wardrive_bridge.c`, CSI setup). Upstream builds with a
newer IDF. See `waveshare-verification.md`.

## Host tests and layout previews (no hardware)

The hardware-independent pieces build and run with the system compiler, from `firmware/`:

```sh
cc -O2 -Wall -Wextra -I main tools/netlog_test.c main/netlog.c -o /tmp/netlog_test && /tmp/netlog_test
cc -O2 -Wall -Wextra -I main tools/gesture_test.c main/gesture.c -o /tmp/gesture_test && /tmp/gesture_test
cc -O2 -I main tools/apcount_test.c main/apcount.c -o /tmp/apcount_test -lm && /tmp/apcount_test   # upstream's
```

The first two print `all checks passed` and exit 0, or name each failed check and exit 1.

The screens render to PPM images with the firmware's own drawing code; `-DLCD_WATCH=1` adds the watch's pages
(`w*-410x502.ppm`, PNG copies in `docs/previews/`):

```sh
cc -O2 -DLCD_WATCH=1 -I main tools/lcd_preview.c main/lcd_draw.c -o /tmp/lcd_preview && /tmp/lcd_preview /tmp/out
```

## Watch fonts

`main/lcd_fonts_watch.h` comes from `tools/fonts/gen.py <font dir> main/lcd_fonts_watch.h watch` (needs Pillow, plus
`Orbitron[wght].ttf`, `Rajdhani-Bold.ttf` and `ShareTechMono-Regular.ttf` from github.com/google/fonts `ofl/`). Leave
`main/lcd_fonts.h` alone: today's font files and Pillow don't reproduce it byte for byte, so regenerating it would
shift the other boards' screens.

## Updates from the app

Wardrive Go and the rocketgod.dev flasher install the **published** ESP32-S3 image, which doesn't yet carry the
watch support. An update from either removes the screen until this firmware is flashed again
(`waveshare-port-analysis.md` R4).
