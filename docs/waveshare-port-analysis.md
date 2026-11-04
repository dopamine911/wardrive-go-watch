# Waveshare ESP32-S3 Touch AMOLED 2.06 — WardriveESP port analysis (Phase 1)

Status: investigation only. No firmware modified.
Date: 2026-11-03.

Written against WardriveESP 1.7.5. The firmware has since moved to 1.7.6 (verification "WardriveESP 1.7.6"): line
numbers and the strip height below describe 1.7.5.

## 0. Findings that change the plan

Ordered by impact. Confidence in brackets.

1. **There is no public Wardrive Go source for the WardriveESP protocol.** [0.95]
   `github.com/RocketGod-git/wardrive-go` HEAD `b30f16f` (pushed 2026-08-24, `versionName 2.0.34`) contains no
   service UUID `47686F73-…`, no `WardriveESP`/`iosESP` name, no `wdstream`, no `WD:` parsing (grep over a fresh
   clone: zero hits). WardriveESP 1.0 shipped 2026-10-05, after that push. The app that speaks the protocol is
   closed source: it ships only through Google Play (`com.rocketgod.wardrive`), the App Store (`id6783390938`) and
   iOS TestFlight 2.23 (2) (`ref/RocketGod-git.github.io/WardriveGo/ios-version.json`). The site repo
   `ref/RocketGod-git.github.io` (`5fd566a`) has release binaries, `version.json` and the web flasher, but no app
   source. The WardriveESP GitHub repo named in its README (`RocketGod-git/WardriveESP`) does not resolve publicly.
   Consequences:
   - "Mobile-side changes if necessary" (Phase 3) is **blocked**: there is nothing to change.
   - App behaviour can only be inferred from the firmware and `PROTOCOL.md`; it cannot be checked.
   - "Phone GPS status" and "phone-reported session statistics" **do not exist in the protocol** (§3.7), and
     adding them needs an app change nobody here can make.
   - Proving that Wardrive Go ingests the watch's observations needs the store build of the app plus the hardware.
2. **WardriveESP has no build-time board targets.** [0.95] Each chip gets one image. Boards are recognized at
   runtime from a hardware fingerprint (`board_detect()`, `wardrive_bridge.c:3016`). A dedicated
   `waveshare_s3_amoled_206` *build target* would add a second convention. Decision D1 below.
3. **On ESP32-S3, every board-specific subsystem is compiled out.** [0.95] The LED/alert path, battery gauge, I2C,
   color LCD and microSD runs are all behind `#if CONFIG_IDF_TARGET_ESP32C5` (or `C5 || C6`). On S3,
   `led alert …` (the phone's alerts) answers `no LED ring on this board` (`wardrive_bridge.c:2818`).
4. **The default GPS pin collides with the display.** [0.85] The default GPS UART RX is GPIO 4
   (`Kconfig.projbuild`, `WARDRIVEESP_GPS_RX_GPIO`), and `gps_task` runs on every board (`wardrive_bridge.c:840-879`).
   GPIO 4 is AMOLED QSPI D0 on this board. `board_pin_busy()` (`:2916`) only knows the C5 Crumb's gauge pins. As
   shipped, the stock S3 image would put UART RX on the display's data line.
5. **The display driver in the vendor code is not what the task prompt assumes.** [0.8] The README and every
   Arduino example say CO5300 (`Arduino_CO5300(…, 22 /* col_offset1 */ …)`). The ESP-IDF BSP drives the panel
   with `esp_lcd_new_panel_sh8601()` and its own init table (`esp32_s3_touch_amoled_2_06.c:70`, `:646`). Reading:
   the panel is a CO5300, the generic QSPI SH8601 driver is used because the command sets are compatible, and the
   vendor table supplies the CO5300 settings. That is an inference, not something the repo states.
6. **The physical board on this machine has 32 MB flash, not 16 MB.** [0.9] Every Waveshare `sdkconfig.defaults`
   configures 16 MB. `esptool flash_id` on `/dev/ttyACM0` returned ESP32-S3 (QFN56) rev v0.2, embedded 8 MB PSRAM,
   flash `c8/4019`, 32 MB, which matches the README. The user confirmed (2026-11-03) that this device is the watch.
7. **Scanning only runs while a host holds the board.** [0.95] `scan_wanted()` = `host_linked() || sdrun_active()`
   (`:256`). A BLE disconnect calls `stream_stop()` (`:4583`). With no phone, the watch does not scan. The
   requirement "keep scanning while the screen is off" is met (the screen has no bearing on it). "Continuously scan"
   and "scan start/stop on the watch" conflict with the current ownership model (§6, R7).
8. **microSD runs are designed for boards with GPS only.** [0.9] `PROTOCOL.md:310-311` and `sdrun.h`: rows need
   the board's own GPS position. The watch has no GPS. `sdrun.c` is also SPI-only and stubbed on S3
   (`sdrun.c:13-38`), and it hard-codes `SPI2_HOST` (`sdrun.c:129-144`), which is the display's QSPI host.
9. **Official flashing tools pick the image by chip only, and erase the whole flash.** [0.9] The web flasher maps
   chip to image (`ref/RocketGod-git.github.io/WardriveESP/index.html:270-275`, `:754-758`) and writes with
   `eraseAll = true` (`:764`). Erasing clears NVS, including a stored `board <id>`. The Marauder Mini v3 needed a
   special case in the flasher to put its identity back (`:597-613`, `:635-641`). The download step is "sanity-checked
   like the app does" (`:449`), so the app's USB flasher is probably also per chip [INFERENCE].
   Consequences for D1:
   - A separate watch image gets replaced by the stock S3 image on any official flash or update.
   - An identity that lives only in NVS is wiped by every flash.
   - Only a hardware fingerprint in the stock S3 image survives both.
   - The flasher's `verifyWardrive()` (`:618-653`: `wdusb hello`, parse `WD:CAPS`, `wdusb bye`) is a public host-side
     protocol client. Acceptance check 4 (§8) uses the same procedure.

## 1. Sources and provenance

| Source | Revision | Receipt |
|---|---|---|
| WardriveESP | 1.7.5 (`FW_VERSION`, `wardrive_bridge.c:82`) | `ref/wardrive-bridge-package/source` is byte-identical to the current `rocketgod.dev/WardriveESP/WardriveESP.zip` (`diff -rq`, 2026-11-03). `cmp` of that zip against `ref/RocketGod-git.github.io/WardriveESP/WardriveESP.zip`: identical. `version.json` latest = 1.7.5. |
| rocketgod.dev site | `5fd566a` (2026-11-03) | `ref/RocketGod-git.github.io`; web flasher, bins, `version.json`; no app source |
| Waveshare repo | `30f3b23` | `ref/ESP32-S3-Touch-AMOLED-2.06` |
| Wardrive Go | `b30f16f`, 2.0.34 | fresh clone, `/tmp/wardrive-go`; finding 0.1 |
| ESP-IDF | v5.5.2 (`~/esp/esp-idf`) | WardriveESP README asks for 5.5 |
| Hardware | ESP32-S3 rev v0.2, 8 MB PSRAM, 32 MB flash | `esptool flash_id`, `/dev/ttyACM0` (`303a:1001`) |

**Baseline build** (stock source, copied to `/tmp/wd-baseline`, `idf.py set-target esp32s3 && idf.py build`):
- Exit 0.
- `wardrive_bridge.bin` = 0xff420 (1,045,536 B), which leaves 0xe0be0 (47 %) of the 0x1E0000 app slot free.
- 3 warnings, all existing ones (`-Wunused-function`): `display.c:127 text_c`, `wardrive_bridge.c:2816 led_apply`,
  `:2502 board_is_awok`.
- Our build differs in size from the shipped `firmware-bins/esp32s3/wardrive_bridge.bin` (1,104,256 B). The cause
  was not investigated (possibly the IDF patch level or a dev `-sd` config). This matters for the
  "no new warnings / no protocol change" comparison: compare against our own baseline, not the shipped bin.

All `file:line` below are relative to `ref/wardrive-bridge-package/source/main/` unless prefixed.

## 2. WardriveESP architecture (answers to Phase 1 questions 1–8)

### 2.1 Framework, build, dependencies, S3 configuration
- **Framework and build:** pure C on ESP-IDF, CMake, built with `idf.py set-target <chip>`. One component, `main`.
  `REQUIRES bt esp_wifi … esp_driver_spi esp_driver_i2c esp_driver_ledc … fatfs sdmmc esp_driver_sdspi`
  (`CMakeLists.txt`).
- **Managed dependencies:** none (`idf_component.yml`: `idf >=5.1`). LCD panels are driven with raw
  `spi_master` + DCS commands (`lcd.c:133-139`), not `esp_lcd`.
- **`sdkconfig.defaults`:**
  - BLE: NimBLE, 1 connection, preferred MTU 247.
  - SW coexistence on.
  - 4 MB flash, custom `partitions.csv` (two 0x1E0000 OTA slots), rollback on.
  - No PSRAM.
  - No console setting, so on S3 the console is UART0. `UART_LINK` = 1 (`:98-104`) and `USB_JTAG_LINK` = 1 (`:97`).
- **S3 specifics in the code:**
  - CSI is off (`:90-94`).
  - No co-processor.
  - Only bench `pinsense` has an S3 pin list (`:2931-2932`).
  - Everything else board-specific is C5/C6 only (finding 0.3).

### 2.2 Hardware target selection
- Runtime fingerprinting: `board_init()` (`:3165`) probes parts and then `board_detect()` (`:3016`) maps them to
  `board_t` (`:2478-2480`, table `BOARDS[]` `:2482-2500`). Only C5 boards have probes today.
- The identity surfaces in three places:
  - `WD:CAPS board=<id> board_name=<Name>` (`board_caps_set`, `:3155`).
  - Advert-extras frame A byte 1 = `board_t` value (`ext_build`, `:4445`); `PROTOCOL.md:55` lists 0–9.
  - The advert "screen fitted" bit (`:4437-4438`).
- `board <id>` in NVS (`:3225`) is the escape hatch for parts that can't be read back. It is still gated on the hardware.
- Rule in the source comments (`:2476-2477`, `:2997-2998`): pins are only driven for a board identified by a
  fingerprint. Generic boards touch nothing.

### 2.3 Display rendering abstraction
There are two independent display modules. Neither is generic over size.

- **`display.c` / `display.h`:** SSD1306 128×32 over I2C (Poltergeist). Fed by `display_fill()` (`:3120`) into
  `display_status_t`.
- **`lcd.c` / `lcd.h` + `lcd_draw.c` / `lcd_draw.h`:** SPI color panels (ILI9341 240×320, ST7735 128×128).
  - Rendering is split cleanly:
    - `lcd_draw_strip(uint16_t *strip, int y0, const lcd_frame_t *f)` is plain C with no IDF calls. It draws
      16-row strips (`LCD_SH`) of RGB565, byte-swapped for the wire, using anti-aliased fonts (`lcd_fonts.h`).
    - `lcd.c` owns the bus, a single DMA strip buffer (`w*16*2` bytes, internal RAM, `:387`), and per-strip FNV
      hashes. A strip is only sent when it changed (`render`, `:306-320`).
  - **Task model:** `wd_lcd`, priority 1, 4 KB stack (`:414`). One frame every 200 ms (`FRAME_MS`). State is pulled
    once per frame through the `lcd_fill` callback (`:3135`), so the scan path never waits on the display.
  - **Hooks from the radio path:** `lcd_note_ap()` (`:286`) only bumps a byte in a channel-activity array.
  - **Screen-off control:** `lcd_set_stealth()` (`:448`) and an idle dim after 30 s with no host (`:41`, `:379`).
  - **Hard limits for a 410×502 panel:**
    - `LCD_W 240` / `LCD_H 320` size the strip-hash table (`lcd_draw.h:11-14`).
    - Scene coordinates are literal 240-wide pixel positions (for example `lcd_draw.c:257` `222 - …`,
      `:343` y=314).
    - Pins are fixed `#define`s (`lcd.c:27-36`).
    - The panel model assumes a PWM backlight GPIO (`panel_t.bl`, `lcd.c:97`, LEDC `:395-399`).
    - The probes compile to `return false` off C5 (`lcd.c:221`, `:251`).
    - AMOLED has no backlight: brightness is DCS `0x51`.
- **Host preview:** `tools/lcd_preview.c` renders the scenes to PPM on a PC with the firmware's own `lcd_draw.c`.
  That is an existing simulator path for dashboard work.

### 2.4 Wi-Fi scan scheduling and buffering
- **`scan_task`** (`:994`, priority 5, 10 KB stack, created by `wifi_start` `:1022`).
  - It loops over the channel plan while `s_streaming && scan_wanted()`.
  - Each channel gets one blocking `esp_wifi_scan_start(&cfg, true)` (`scan_one_channel`, `:914`): active on
    2.4 GHz (default dwell, since coex rejects custom active times), passive 120 ms on 5 GHz/DFS. The S3 is
    2.4 GHz only.
  - Then 5 ms sleep.
  - It emits `WD:STATUS aps= scans=` per sweep (`:1013`).
- **Buffering:** none of its own. Up to 60 records per channel (`MAX_AP_PER_SCAN`) are `calloc`ed, sent
  synchronously as `WD:AP` lines from the scan task, then freed (`:929-934`). This is a per-channel heap
  alloc/free, which matters for fragmentation over hours (R9).
- **Counters** (`apcount.c`) are fixed memory (2.5 KB of HyperLogLog sketches), lock-free single-byte registers.
  - Session-unique count: about 3 % error.
  - Live count (last 20–30 s): about 4 % error.
  - Host-tested by `tools/apcount_test.c`.
- **Ownership:**
  - Scanning starts when a host subscribes or says hello (`host_joined`, `:2130`; GAP subscribe `:4590-4597`).
  - It stops on disconnect (`:4583`).
  - The channel plan and role reset on every connect and disconnect (`:4554-4558`, `:4584-4585`).

### 2.5 BLE transmission of observations
- **Output path:**
  1. `send_line()` (`:497`) formats into a shared `s_line[384]` under `s_send_lock`.
  2. `emit_locked()` (`:465`) sends to each sink.
  3. `send_chunks_locked()` splits the line into MTU-sized DATA frames with a 12-byte header
     (`send_frame`, `:429-442`; magic `0x47 0x42`, version 1, type, reply id, length).
- **Backpressure** (`tx_room`, `:418-427`): 4 mbufs are kept for GATT responses. Non-host tasks wait up to
  100 ms for room, then drop the frame and count it in `s_tx_dropped`, which is reported as `WD:HEALTH txdrop`.
  The host task never waits.
- **`WD:AP` format** (`send_ap_line`, `:526-544`):
  `WD:AP ts=<ms> bssid=AA:.. ssid_hex=<lowerhex> rssi= ch= auth=<OPEN|WEP|WPA|WPA2|WPA3|WPA2_WPA3|UNKNOWN> hidden=0|1`.
  After sending, the function feeds `apc_add`, `display_note_ap`, `lcd_note_ap` and, during an offline run,
  `sd_note_ap`. **This is the single seam where every observation passes** (Wi-Fi role; capture mode has its own
  `cap_emit_ap`, `:1178`).

### 2.6 BLE characteristics, commands, notifications
- **GATT** (`:4354-4364`, `PROTOCOL.md:5-15`). Service `47686F73-7445-5350-4272-696467655376`:

  | Characteristic | Direction | Properties |
  |---|---|---|
  | RX | phone → board | write / write-no-rsp |
  | TX | board → phone | notify |
  | CTRL | phone → board | write / write-no-rsp |
  | OTA | phone → board | write-no-rsp |

  - The status UUID (`…5354`) is advertised while the board is linked.
  - No encryption, no bonding.
  - Commands are queued from the GATT callback into a 6-slot ring and run after the write response
    (`cmd_drain`, `:4315-4347`).
- **Advertising:**
  - Free board: connectable, the service UUID, and a scan response with the name `WardriveESP-XXXX` plus the
    status bytes (`set_free_fields`, `:4460`).
  - Linked board: non-connectable beacon (`:4479-4514`).
  - Advert extras: 6 bytes under UUID16 `0x5744` (`:4419-4457`). The advert packet is full at 31 bytes
    (`:4412-4413`).
- **Commands** (`exec_command`, `:3514-3590`): `wdstream`, `start`, `stop`, `bt`, `cap`, `caps`, `help`, `info`,
  `status`, `ch`, `reboot`, `link`, `drop`, `bye`, `iam`, `led`, `board`, `screen`, `coproc`, `csi`, `sd`, `time`,
  `ota`. `hb` and `client` are silently ignored (`:3536`).
- **Notifications (`WD:` lines):** `CAPS`, `BOOT`, `CRASH`, `LINK`, `CONN`, `MODE`, `BEGIN`, `PLAN`, `STATUS`,
  `END`, `AP`, `BT`, `GPS`, `HEALTH`, `HS`, `PMKID`, `CLIENT`, `OTA`, `ERR`, `TIME`, `RUNS`, `SCREEN`
  (`PROTOCOL.md:123-157`).

### 2.7 What the phone sends back (statistics, alerts)
Everything the firmware accepts from the phone:

| Command | What it carries |
|---|---|
| `iam a\|i <nickname>` | the holder's identity (`:3538-3546`) |
| `time <epoch>` | clock (`:3553-3557`) |
| `led alert <pattern> <rrggbb> [secs] [LABEL]` | alerts: police, trackers, drones, cameras, captures… (`PROTOCOL.md:655-671`); shown as a banner by `lcd_alert` (`:2808`) |
| `ch <list>` / `wdstream -ch` | channel split across several boards |
| `bye` | the app is releasing the board |
| `hb`, `client` | ignored |

**Not sent:**
- Session totals.
- New-AP counts.
- Phone GPS fix or status.
- Any other statistic.

`PROTOCOL.md:670-671` says the app sends `led` only to boards with `led=` in `WD:CAPS`. Yet the firmware also
accepts `led` when a color LCD runs (`:2758`), and those boards report `disp=` rather than `led=`. That suggests
the app also keys on `disp=`, but the app source is unavailable to confirm it (R3).

### 2.8 Battery, SD logging, display power on existing boards
- **Battery:**
  - MAX17048 at 0x36 on I2C 5/4, C5 only (`:2821-2913`), read every 30 s by `health_task` (`:4806`).
  - Exposed as `s_batt_pct`/`s_batt_mv`: `WD:CAPS batt=1`, `WD:HEALTH batt= batt_mv=`, and advert-extras byte 2.
  - `i2c_master.h` is only included for C5/C6 (`:57-61`).
- **SD:** `sdrun.c` offline WiGLE CSV runs, used only while no host is linked. SDSPI only; slot on the LCD bus or
  `sdrun_init_bus()` on SPI2. Stubbed on S3. GPS-gated by design (finding 0.8).
- **Display power:**
  - Stealth (`led stealth on`, saved in NVS `ledstealth`, `:3204-3209`) sends DCS `0x28` (display off) and turns
    the backlight PWM to 0 (`lcd.c:359-366`).
  - Idle dim to 40/255 after 30 s with no host.
  - No touch input exists anywhere in the firmware.

## 3. Waveshare reference (question 9)

All of the following are from `ref/ESP32-S3-Touch-AMOLED-2.06` and checked by grep. "BSP" means
`examples/factory/components/waveshare__esp32_s3_touch_amoled_2_06/`.

**Panel**
- 410×502 QSPI AMOLED. CO5300 per `README.md:33` and the Arduino examples. Driven by `esp_lcd_sh8601` in the BSP
  (`esp32_s3_touch_amoled_2_06.c:646`).
- Pins: CS 12, PCLK 11, D0–D3 4/5/6/7, RST 8 (`board_config/pin_config.h`, BSP `.h:48-55`). No backlight GPIO
  (`BSP_LCD_BACKLIGHT NC`).
- Host `SPI2_HOST` (BSP `.h:259`).
- RGB565 big-endian (`display.h:13`). The existing `lcd_draw` byte-swapped output fits this.
- Column gap 22 (`esp_lcd_panel_set_gap(panel, 0x16, 0)`, `:649`; Arduino `col_offset1 = 22`).
- The vendor init table at `:70-…` sets brightness `0x51` to 0x00. AMOLED emission stays off until the first frame
  is drawn.
- The LVGL rounder forces even x/y start and odd end, i.e. even-sized windows (`:621-634`). The CO5300 needs
  2-pixel-aligned windows. Strips of 16 rows across the full 410-pixel width already satisfy this.
- The pixel clock lives in the managed `esp_lcd_sh8601` macros, which are not vendored. **Unknown locally.**

**Touch**
- FT3168 at 0x38 on I2C SDA 15 / SCL 14, INT 38, RST 9 (`pin_config.h:15-18`, BSP `.h:56-57`).
- The BSP uses `esp_lcd_touch_ft5x06` (protocol-compatible).
- No swap or mirror.

**I2C**
- SDA 15 / SCL 14. Also on the bus: AXP2101 (0x34), PCF85063 RTC, QMI8658 IMU, ES8311 / ES7210 codecs.
- The address list beyond 0x34 and 0x38 comes from the subagent's reading of the schematic. Not re-verified.

**PMIC**
- AXP2101 via XPowersLib (C++), `examples/esp-idf/01_AXP2101/main/port_axp2101.cpp`:
  - `disableTSPinMeasure()` at `:114`. Without it the battery reading is wrong when there is no NTC.
  - `getBatteryPercent()` at `:140`.
  - `isCharging()` at `:158`.
- The rail-setting calls in that example are all commented out (`:46-81`), so the display runs from the PMIC's
  default rails.

**microSD**
- 1-bit SDMMC: CLK 2, CMD 1, D0 3, plus "CS" 17 (`pin_config.h:22-25`, BSP `.h:60-62`).
- In SPI mode that maps to SCK 2 / MOSI 1 / MISO 3 / CS 17. That mapping is an inference from standard SD pinout,
  not confirmed in the repo.

**Memory**
- 8 MB octal PSRAM (examples `CONFIG_SPIRAM_MODE_OCT`; esptool: embedded 8 MB).
- 32 MB flash (README, esptool).

**USB and console**
- Native USB-Serial-JTAG (esptool `USB mode: USB-Serial/JTAG`).
- The examples log on UART0 at 2 Mbaud.

**Buttons**
- BOOT is GPIO 0.
- PWR is GPIO 10 according to the subagent's schematic reading only. **Not in any code; unverified.**

**Coexistence evidence**
- The factory firmware runs Wi-Fi, display and audio together. It uses no BLE, so there is **no vendor evidence for
  BLE + Wi-Fi + QSPI together**.

### Hardware differences vs. existing WardriveESP boards

| Aspect | Existing color boards (C5) | Watch (S3) |
|---|---|---|
| Panel bus | 1-bit SPI, 20 MHz, DC pin | QSPI, no DC pin (commands in the QSPI header) |
| Brightness | LEDC PWM backlight | DCS `0x51`, panel on/off `0x28`/`0x29`, sleep `0x10`/`0x11` |
| Size | ≤ 240×320 | 410×502, column offset 22 |
| Touch | none used | FT3168 I2C |
| Battery | MAX17048 (C5 Crumb/Koko) | AXP2101 PMIC |
| SD | SDSPI on the LCD bus | SDMMC 1-bit pins, its own lines; SDSPI would need `SPI3_HOST` |
| GPS | some boards | none |
| Radio | C5 dual-band | 2.4 GHz only |
| PSRAM | none assumed | 8 MB octal (not needed for strip rendering) |

## 4. Mapping the requested dashboard to available data

| Requested | Source today | Note |
|---|---|---|
| BLE connection status | `s_conn_handle`, `s_holder` | local |
| Total discovered APs | `apc_session()` | local, unique since boot, ~3 % error. **Not the phone's total** |
| New APs this session | `apc_session()` minus a snapshot taken at connect | local; trivial |
| Currently visible APs | `apc_live()` | local, last 20–30 s |
| Current channel | `s_crumb_ch` | local |
| Phone GPS status | — | **not in protocol** (finding 0.1). Only board GPS (`s_gps_present`) exists, and the watch has none |
| Battery % | — | needs an AXP2101 reader (new) |
| Scan activity | `s_streaming`, `s_crumb`, `s_sweeps` | local |
| Recent networks (SSID / BSSID / RSSI / ch / auth) | `send_ap_line` arguments | needs a new bounded ring fed at that seam |
| Brightness, timeout | — | new, local; NVS like `ledstealth` |
| Firmware info | `FW_VERSION`, `esp_ota_get_running_partition` | local |
| Phone alerts | `led alert …` → `lcd_alert` | exists; S3 guard must open (finding 0.3) |

The only phone-originated data is alerts and `iam`. The protocol has a `time` command, but Wardrive Go doesn't send it
(observed 2026-11-03, verification O13).

## 5. Proposed modifications (minimal; awaiting D1–D4)

- **Rendering:** reuse `lcd_draw`'s strip/font/glyph primitives and the `lcd.c` frame loop pattern (pull state
  once per frame, hash strips, send only changed strips). Add a watch-specific panel back end and scene. Don't
  migrate to LVGL: the existing renderer already meets the constraints (no PSRAM, no full framebuffer, 13 KB DMA
  strip, low-priority task) and LVGL would add a large managed dependency to a project that has none.
- **Board identity:** add `BOARD_WAVESHARE_AMOLED_206` at the end of `board_t`, so all existing values keep their
  numbers. Its `BOARDS[]` entry is id `waveshare_s3_amoled_206`. Include it in the "screen fitted" bit.
- **Pin protection:**
  - Extend `board_pin_busy()` so the GPS probe skips GPIO 4–8 and 11–12 (QSPI), 14–15 (I2C) and 38 (touch INT)
    on this board.
  - Decide whether `gps_task` should run at all on the watch.
  - Fixes finding 0.4.
- **Alerts:** open the S3 path in `cmd_led` (or factor `cmd_led`'s screen branch out of the C5/C6 LED block), so
  `led alert` reaches the watch's banner.
- **Battery:** read the AXP2101 for `s_batt_pct`/`s_batt_mv`. This reuses `s_has_batt`, so `WD:CAPS batt=1`,
  `WD:HEALTH batt=` and the advert byte all start working with no protocol change.
- **Touch:** read the FT3168 through `esp_lcd_touch_ft5x06`, or with direct register reads on the shared I2C bus.
  Polled from the display task (no extra task). Gesture filtering: require a press/hold, not a single tap
  (Phase 4 "accidental input").
- **Screen power:** use DCS `0x28` with brightness `0x51`=0, or sleep `0x10`. Wake on touch INT or the BOOT
  button. Scanning and BLE are independent of the display task already.
- **Recent-networks ring:** a fixed array of N records (bounded, no heap), written from `send_ap_line` with a
  `portMUX` or seqlock and read by the display task.
- **Diagnostics:** most already exist in `WD:HEALTH` (`heap`, `heap_min`, `largest`, stack watermarks, `txdrop`,
  `mtu`), plus `link`/`status`. Missing: APs/s throughput and per-frame render time. Add them only to the local
  screen or log (no new `WD:` keys without D3).
- **No changes** to the GATT table, UUIDs, frame format, `WD:AP` format, advert layout, or partition layout.

## 6. Risks and unsupported assumptions in the task prompt

| # | Risk / assumption | Evidence | Mitigation |
|---|---|---|---|
| R1 | App ingestion cannot be verified without the closed app | finding 0.1 | Need a phone with the store Wardrive Go build; USB `wdusb hello` session gives a host-side protocol check meanwhile |
| R2 | Phone stats and GPS on the watch | §2.7 | Show only local stats, labelled "local"; a protocol extension is blocked on the app |
| R3 | App may not send `led alert` to a board without `led=` | `PROTOCOL.md:670-671` | **Resolved 2026-11-03:** the app sent `led alert spin FFD23B 6 TRACKER` to the watch (`disp=410x502`, no `led=`). The S3 `cmd_led` stub is now the only blocker (`waveshare-verification.md`) |
| R4 | Official tools overwrite any watch-specific image with the stock S3 image | web flasher: per-chip `BINS`, `eraseAll` (finding 0.9); README "updates install over Bluetooth"; app compares numeric versions (`:83-84`) | Until upstream ships the watch support, any flash or update from rocketgod.dev or the app removes it, whichever D1 option is chosen. (a) survives once upstream merges it; (b) never does |
| R5 | Unknown `board_t` value 10 in advert extras | `PROTOCOL.md:55` lists 0–9 | No fault seen: the Android app links, streams and relinks with board 10 (`waveshare-verification.md` #7, #10). iOS untested |
| R6 | Unknown `disp=410x502` | `send_caps_to` `:575-576` | Same as R5 |
| R7 | "Scan start/stop" on the watch contradicts phone ownership | `scan_wanted()` `:256`; the app drives `wdstream` | Implemented as pause/resume through the same `stop`/`start` commands (user decision). **Checked 2026-11-03:** Wardrive Go (Android) stopped with the watch, stayed stopped, and resumed on the watch's resume (`waveshare-verification.md` #27) |
| R8 | GPS probe on GPIO 4 corrupts the display bus | finding 0.4 | `board_pin_busy` |
| R9 | Heap fragmentation from a per-channel `calloc` of up to 60 × `wifi_ap_record_t` | `:929` | Watch with `WD:HEALTH largest=` over a multi-hour run before changing upstream code |
| R10 | QSPI sharing `SPI2_HOST` with SDSPI | `sdrun.c:137` | SD on `SPI3_HOST` or SDMMC; out of the first slice |
| R11 | No vendor evidence for BLE + Wi-Fi + QSPI rendering together | factory firmware has no BLE | Hardware soak test |
| R12 | CO5300 timing and pixel clock unknown locally | managed component not vendored | Fetch `waveshare/esp_lcd_sh8601` from the registry at implementation time and read its QSPI config |
| R13 | Image size: fonts for 410×502 | 47 % of the slot free | Measure each step against the 0x1E0000 slot |

## 7. Decisions (made 2026-11-03)

- **D1, board model:** a runtime fingerprint inside the stock S3 image. Board id `waveshare_s3_amoled_206`. No
  separate build: it follows upstream convention, survives the flasher's full erase (finding 0.9), and is the only
  form upstream could ship.
  - **Revised 2026-11-03 after the first boot on hardware.** The FT3168 at 0x38 stays silent until its reset line is
    pulsed, so it can't be part of a check that drives no pins.
  - The fingerprint is now: I2C SDA 15 / SCL 14 both pulled up, the AXP2101 at 0x34 reading IC type 0x4A, and the
    other board-bus parts 0x18 / 0x40 / 0x51 / 0x6B all answering.
  - The user deferred the choice; this option was picked because it drives no pin before identification.
- **D2, repo layout:** WardriveESP 1.7.5 is vendored into `firmware/`. The first commit is pristine, so the port
  is a reviewable diff on top of it. `ref/` stays untouched.
- **D3, protocol:** no new `WD:` lines, keys or commands. Only the values of the existing `board=`, `board_name=`,
  `disp=` and `batt=` keys change. Watch-only statistics stay on the screen and in the serial log.
- **D4, offline SD logging:** out of scope. There is no GPS, and upstream's design excludes boards without one.
- **End-to-end check:** the user has an Android phone with the store build of Wardrive Go.

## 8. Acceptance criteria for vertical slice 1

1. `idf.py build` for esp32s3 exits 0 with no warnings beyond the 3 baseline ones. The C3/C5/C6 builds also exit 0.
2. On the watch, the serial log shows `board: Waveshare ESP32-S3 Touch AMOLED 2.06` and no GPS probe on GPIO 4.
3. The display shows a dashboard with BLE state, live/session AP counts and the channel, updating at least once
   a second while scanning.
4. A USB `wdusb hello` session receives `WD:CAPS … board=waveshare_s3_amoled_206 …` and a stream of `WD:AP` lines
   in the documented format (host-side protocol check, no app needed).
5. Wardrive Go (store build) on a phone discovers `WardriveESP-XXXX`, links, and shows APs. This needs the phone;
   it is reported separately and never inferred.
6. `WD:HEALTH` over 30 minutes of concurrent scanning and rendering: `txdrop` doesn't grow faster than baseline,
   `heap_min` and `largest` are stable.
