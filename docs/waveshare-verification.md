# Waveshare watch port: verification record

Every result is labelled with the kind of evidence behind it:

| Label | Meaning |
|---|---|
| **BUILD** | compiler output |
| **AUTO** | host tests and render comparisons on the PC (`firmware/tools/`) |
| **HW-LOG** | the watch's serial log or USB protocol output |
| **HW-EYE** | the user looking at the watch or the phone |
| **INFER** | reasoning, not observed |

Nothing here comes from a simulator. Host tests cover only the hardware-independent modules.

## Slice 1: boot, Wi-Fi scan → Wardrive Go, live screen (2026-11-03)

**Setup**
- **Firmware:** `firmware/`, WardriveESP 1.7.5 plus the slice-1 diff.
- **Toolchain:** ESP-IDF v5.5.2.
- **Watch:** ESP32-S3 rev v0.2, 8 MB PSRAM, 32 MB flash, on `/dev/ttyACM0`. The device on that port was confirmed
  to be the watch by the user.
- **Phone:** Pixel ("Pixel11Pro" in its `iam` line), Wardrive Go store build, Android.

### Results

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | esp32s3 builds | pass | BUILD: exit 0; same 3 `-Wunused-function` warnings as untouched upstream; 0x123220 bytes, 39 % of the slot free (upstream 0xff420) |
| 2 | esp32c3, esp32c6 still build | pass | BUILD: exit 0, warnings identical to untouched upstream; +0x60 bytes each |
| 3 | esp32c5 still builds | **blocked: fails on untouched upstream too** | BUILD: untouched upstream fails on IDF v5.5.2 (`lltf_bit_mode`, CSI). With that one line removed in a throwaway copy, upstream and ours both build with identical warnings (+0xA0 bytes) |
| 4 | Watch identified from hardware | pass | HW-LOG: `watch probe: AXP2101 IC type 4a, 4 of 4 board parts answered`, `board: Waveshare AMOLED 2.06` |
| 5 | GPS probe kept off the display's data pin | pass | HW-LOG: `GPS: GPIO 4 belongs to this board's own hardware, GPS off`. Before the fix, on the generic path: `listening on GPIO 4` |
| 6 | Display starts and shows the live dashboard | pass | HW-LOG: `lcd: 410x502 CO5300 running`. HW-EYE: two photos of the dashboard centred on black with correct colours. One shows a 1:38 clock, 6 in range, 7 total, channel 8, activity bars |
| 7 | Discoverable, links, relinks | pass | HW-LOG: `advertising as 'WardriveESP-476A'`, `phone connected`, `MTU 247`, `notifications on`, `wdstream start -wifi`, `iam a Pixel11Pro`. The phone relinked by itself after every reset and after a USB session |
| 8 | Observations in the existing format | pass | HW-LOG (USB session): 74 `WD:AP` lines from 7 BSSIDs in 25 s, e.g. `WD:AP ts=403725 bssid=A8:40:41:32:96:CC ssid_hex=… rssi=-67 ch=11 auth=WPA2 hidden=0` |
| 9 | Identification in the existing caps line | pass | HW-LOG: `WD:CAPS fw=1.7.5 chip=esp32s3 bands=2g ota=1 bt=1 gps=0 cap=1 usb=1 csi=0 board=waveshare_s3_amoled_206 board_name=Waveshare_AMOLED_2.06 disp=410x502 link=usb chans=…` |
| 10 | Wardrive Go ingests the watch's networks | pass | HW-EYE: user saw this board's networks arrive in the app while the watch counts climbed (Wi-Fi mode) |
| 11 | Keeps running with the app in the background | pass | HW-EYE: clock kept counting with the app backgrounded for about 60 s. HW-LOG: continuous from uptime 180 s to 384 s, no reboot |
| 12 | Memory and link health | pass (one sample) | HW-LOG: `WD:HEALTH up=417 heap=160564 heap_min=95068 largest=53248 stack_scan=7180 … txdrop=0 temp=42` |

### Observations that change the plan

- **The FT3168 touch chip doesn't answer before its reset is pulsed.** HW-LOG: first boot scanned
  `0x18 0x34 0x40 0x51 0x6b`, with 0x38 silent, and the watch came up as `generic`. The fingerprint now uses the
  AXP2101 chip type plus the four parts that do answer (analysis §7 D1, revised). Touch input will need a reset
  pulse on GPIO 9 after identification.
- **Wardrive Go sends alerts to the watch.** HW-LOG: `cmd: 'led alert spin FFD23B 6 TRACKER'`. At slice 1, `cmd_led`
  on S3 was still the upstream stub, so no banner showed. Fixed in Phase 2–3 (#17). This settles analysis R3.
- **Opening the serial port resets the watch** unless RTS drops before DTR. A reset drops the phone link. Use
  `idf.py monitor --no-reset` (checked: uptime 495 s → 505 s with no reset).

### Open issues

| # | Issue | Status | Evidence |
|---|---|---|---|
| O1 | One screen freeze plus the watch dropping off USB | **not reproduced** | HW-EYE: dashboard frozen at clock 0:04 (about 6 s after boot) and the watch absent from `lsusb` while plugged in. It recovered after the app was foregrounded and the cable replugged. This happened right after a log capture that reset the watch and then closed the port. The freeze did not recur with the app backgrounded for 60 s (#11). The kernel log is unreadable (user not in `adm`). INFER: tied to the bench reset or port close, not to the firmware's normal running. Not proven. |
| O2 | Screen setup holds up boot for about 1 s before Wi-Fi starts | known | HW-LOG: `board:` at 568 ms, `lcd … running` at 1678 ms (1.4 s at slice 1; the separate blanking pass went with the native layout). Same synchronous pattern as the upstream color screens |
| O3 | Dashboard is the 240×320 layout, centred | **closed** (Phase 4) | Native 410×502 pages, #21–#29 |
| O4 | Screen brightness and timeout controls, on-watch scan start/stop | **closed** (Phase 4) | #25–#27 |
| O7 | Screen sleep doesn't wake for an incoming firmware update | known | While dark the screen task doesn't gather state, so the OTA progress screen first shows on the next touch or alert. The update itself is unaffected |
| O8 | Battery charge state | closed (screen only, #52–54) | Shown on the watch from the AXP2101's status registers. Still not sent to the app: no existing `WD:` key carries it (D3). All three states seen on hardware: charging, on battery, and on USB while full (#59) |
| O5 | Sustained soak (≥30 min concurrent scan and render) | partly run | Stress scenarios passed (#56: 67 min of mode switches, alerts, reconnects and touch); the four 30-minute soaks are not run yet (#60) |
| O6 | esp32c5 can't be built with the local IDF | upstream/toolchain | #3; needs the IDF release upstream uses |
| O9 | Drawing a watch frame is slow on the chip | improved (#57–58) | Measured, not guessed: painting is CPU-bound (`memory-soak-results.md` section 4); the earlier INFER of flash-cache misses on fonts was wrong (glyphs are ≈11 % of the work). Alert worst frame 461 → 211 ms, dashboard paint 186 → 156 ms (`-Og`) and lower at `-O2`. Still above upstream's 125 ms alert-blink step (148 ms) |
| O10 | Long serial captures lose stretches of log | not reproduced | Captures of 10–26 min on 2026-11-03 delivered every `MD` line (sequence numbers contiguous; they go through the USB driver) and every expected log line (e.g. 102 of 102 mode switches, 21/21/21 connect lines). Earlier losses stay unexplained |
| O11 | A phone linked but not subscribed (Wardrive Go in the background) never starts the scan | upstream behaviour, now visible | HW-LOG: `phone connected` with no `notifications on` until the app was opened. The watch shows SCAN `WAITING` / `OPEN THE APP` for that state (#28) |
| O12 | Screen task stack headroom | closed (#55) | Measured worst case was a MODE/BOOT press running the role switch on `wd_lcd` (1,060 of 4,096 left); moved to `wd_health` (`ea697af`). Lowest since, across swipes, settings holds, alerts and presses with the log line on: 1,604 |
| O13 | Wardrive Go doesn't send the time | upstream/app behaviour | HW-LOG: 40 s after the app linked, its commands were `wdstream`, `iam` and nothing else (no `time`). The chip starts blank, so the clock face reads NO TIME YET until the clock is set once over USB (docs/waveshare-build-flash.md "Set the clock"). After that the chip keeps it across resets. Not tested, and unknown: whether it keeps time when the battery runs flat and USB is out (which rail powers it wasn't read from the schematic) |
| O14 | Timezone is compiled in | design limit | The chip and the board's clock hold UTC; the face shows it in `WATCH_TZ` (default America/Chicago, `CST6CDT,M3.2.0,M11.1.0`, from the host's tz database). Another zone, or travelling, needs the `#define` edited and a rebuild. DST switching is newlib's, not exercised yet (next change: 2027-03-14) |
| O15 | Power use and battery life | not measured | The AXP2101 can't report current; no drain test has run. Plan and options in [issue #2](https://github.com/dopamine911/wardrive-go-watch/issues/2) |

## Phase 2–3: battery, phone alerts, touch, screen sleep (2026-11-03)

Same watch, phone and toolchain as slice 1. Firmware: slice 1 plus this change.

| # | Check | Result | Evidence |
|---|---|---|---|
| 13 | esp32s3 builds | pass | BUILD: exit 0; same known warnings; 0x12ac10 bytes, 38 % of the slot free (the ring driver now links on S3, ~30 KB) |
| 14 | esp32c3, esp32c6, esp32c5 (CSI line removed) | pass | BUILD: exit 0, warnings unchanged; C3/C6 sizes unchanged, C5 +0x1B0 |
| 15 | Battery read | pass | HW-LOG: `battery AXP2101: 100%, 4114 mV (ADC ctrl 01 -> 01)`. The setup write is a no-op on this unit's defaults. Not checked against a meter |
| 16 | Battery in the existing protocol | pass | HW-LOG (USB session): `WD:CAPS … batt=1 …`, `WD:HEALTH … batt=100 batt_mv=4113 …`. The advert extras carry it through the existing battery byte (INFER: same `s_batt_pct` source, not captured over the air) |
| 17 | Phone alert shows on the watch | pass | HW-LOG: `led alert spin 00C8FF 6 DRONE` sent over USB, reply `led spin 00c8ff 6s`. HW-EYE: DRONE banner flashed about 6 s, then the dashboard returned. The app's `led alert …` over BLE runs the same `cmd_led` |
| 18 | Touch controller | pass | HW-LOG: `touch FT3x68: ready (id 03)` after the reset pulse (0x03 = FT3168 per Arduino_FT3x68.h) |
| 19 | Screen sleep and wake on touch | pass | HW-LOG: `screen off (no touch for 30 s)` at 34.2 s, `screen on (touch at 153,466)` at 73.6 s. HW-EYE: went dark, a tap woke it |
| 20 | Scanning and BLE keep going with the screen off | pass | HW-LOG: 86 BLE notifications in the 39.4 s dark vs 99 in 42.6 s lit (about 2.2/s both) |

## Phase 4: the watch's own pages (2026-11-03)

Same watch, phone and toolchain. Firmware: Phase 2–3 plus the native 410×502 pages.

| # | Check | Result | Evidence |
|---|---|---|---|
| 21 | All chips build | pass | BUILD: esp32s3 0x149b10 (31 % free; watch fonts +114 KB, S3 only via `LCD_WATCH`), esp32c3 unchanged, esp32c6 +0x10, esp32c5 (CSI line removed) +0x820; warnings identical to untouched upstream on all four |
| 22 | Host tests | pass | AUTO: `tools/netlog_test.c` (dedupe, eviction of the longest-unheard, newest-first, SSID limits, clock wrap) and `tools/gesture_test.c` (taps ignored, single hold, swipe thresholds and direction, moved finger never holds); 4 of 5 mutants caught, the fifth equivalent. Upstream `tools/apcount_test.c` unchanged |
| 23 | Upstream screens unchanged | pass | AUTO: all 26 upstream 240×320 / 128×128 preview renders byte-identical before and after (including the `rrect` speed-up) |
| 24 | Pages and swipes | pass | HW-LOG: `watch: page 0/1/2` on swipes. HW-EYE: first build "swipes unreliable" (frames took ~200 ms and the finger was only polled between frames); after polling between strips, "reliable now" |
| 25 | Hold-to-press, accidental touches | pass | HW-LOG: `watch: brightness 25%` / `75%` / `100%`, `watch: screen off after … s`. HW-EYE: brightness, timeout and scan changed on a hold; taps and brushes changed nothing |
| 26 | Brightness and timeout persist | pass | HW-EYE: the 15 s timeout set before a reflash still applied after it (screen off at 15 s in later logs) |
| 27 | Scan pause / resume with the app | pass | HW-EYE: the app stopped with the watch, stayed stopped about 20 s, resumed on the watch's resume. HW-LOG: `watch: scan resumed`, `cmd: 'start'`, `stream started` (the pause lines fell in a lost log stretch, O10) |
| 28 | Linked but app not listening | pass | HW-LOG: `phone connected` without `notifications on` until Wardrive Go was opened; then `MTU 247`, `notifications on`, `wdstream start`. The watch now labels that state `WAITING` instead of `PAUSED` |
| 29 | Drawing cost | pass | HW-LOG (5 s windows, dashboard): before, 25 frames per 5 s at 126–134 ms painting each; after (data once a second, touches and alerts at once), 5 frames per 5 s at 81–131 ms; 0–1 ms state, 1–5 ms panel I/O, 2–3 ms touch; display task stack ≥ 1.7 KB left |
| 30 | Panel corners | pass | HW-EYE: a calibration screen of nested outlines showed the visible area's corners at ~110 px radius (90 cut, 110 whole). The layout keeps clear of that curve (`W_CORNER_R`); "nothing cut off" on the watch |
| 31 | Layout previews | pass | AUTO/visual: `docs/previews/*.png` rendered by `tools/lcd_preview.c -DLCD_WATCH=1` from the firmware's own drawing code |

## MODE button (2026-11-03)

A spike first (throwaway build, reverted): the SCAN button cycled modes with the app's own commands. Wardrive Go
accepted watch-initiated switches and sent no mode commands back over 90 s and three full cycles. Then the feature.

| # | Check | Result | Evidence |
|---|---|---|---|
| 32 | All chips build | pass | BUILD: esp32s3 0x149c40 (31 % free); esp32c3, esp32c6, esp32c5 (CSI line removed) unchanged in size; warnings identical to untouched upstream |
| 33 | MODE cycles Wi-Fi → Bluetooth → capture → Wi-Fi | pass | HW-LOG: `watch: mode after WIFI` → `cmd: 'bt on'` → `bluetooth scan started`; `after BT` → `cap on` → `capture started`; `after CAP` → `cap off` → `stream started`. HW-EYE: the MODE label and Wardrive Go (Android) both followed every hold |
| 34 | The app doesn't override the watch's mode | pass (one phone, one board) | HW-LOG: no `bt off` / `cap off` / `wdstream … -wifi` from the app after any watch-initiated switch. Untested: iPhone, several boards linked at once |
| 35 | Layout with four buttons | pass | AUTO/visual: `docs/previews/w5-controls-holding.png`, `w6-controls-no-phone.png` inside the measured 110 px corner curve; MODE dimmed while no host listens |

## Pages follow the mode (2026-11-03)

Found on the watch: in Bluetooth mode page 1 counted Wi-Fi APs (decaying to 0) beside a stale channel and page 2 showed
old networks; in capture mode page 2 froze (capture's `WD:AP`s bypassed the list) and its handshakes, PMKIDs and
clients weren't shown. Now the dashboard and page 2 show what the current mode measures.

| # | Check | Result | Evidence |
|---|---|---|---|
| 36 | Bluetooth mode | pass | HW-EYE: page 1 shows BLE DEVICES LAST 10 S, THIS WINDOW, WINDOWS, STRONGEST, no channel bars; page 2 is a DEVICES list (name or `<no name>`, address, RSSI, public/random). HW-LOG: `bt on` → `bluetooth scan started`, pages 0–2 visited, no fault |
| 37 | Capture mode | pass | HW-EYE: page 1 shows HANDSHAKES, PMKIDS, CLIENTS and the channel bars; page 2 NETWORKS fills from capture's AP sightings. HW-LOG: `cap on` → `capture started`; `lcd: 84 frames in 60 s, avg 164 ms` while capturing |
| 38 | Builds and previews | pass | BUILD: esp32s3 0x149f90; esp32c3/c6/c5 unchanged in size and warnings. AUTO: `tools/netlog_test.c` passes; `docs/previews/w11-dashboard-bt.png`, `w12-devices-bt.png`, `w13-dashboard-cap.png` |

## Watch face, BOOT button (2026-11-03)

Page numbers from here: 0 clock, 1 dashboard, 2 networks, 3 controls. Rows #24 and #36–37 describe the earlier
three-page layout (dashboard 0, networks 1, controls 2). Asked for: a plain watch screen (time, date, one small counter
for the mode) and the BOOT button to step the mode.

| # | Check | Result | Evidence |
|---|---|---|---|
| 39 | Clock-chip register conversion | pass | AUTO: `tools/rtc_pcf_test.c` (a known epoch ↔ registers, leap day, 2000, year end, a round trip every 7 h 13 min 11 s for 25 years, years outside 2000–2099 refused, oscillator-stopped flag, blank and garbage registers). Not mutation-tested |
| 40 | The clock chip starts blank | pass (expected) | HW-LOG: `clock chip holds no time yet (reads b4 20 19 01 06 01 00)`: oscillator-stopped flag set, year 2000 |
| 41 | Seeded over USB, kept across a reset and a reflash | pass | HW-LOG: `wdusb hello`, `time <epoch>` → `WD:TIME now=1793736556 set=1` (host 1793736557); after a reset `clock set from rtc: 1793736565` at uptime 0.57 s (host 1793736575 ten seconds later); after two later reflashes `clock set from rtc: 1793737012` |
| 42 | Clock face | pass | HW-EYE: the first page shows the right local time and date and a small WI-FI counter, no WARDRIVE header. AUTO/visual: `docs/previews/w14-clock.png`, `w15-clock-bt-midnight.png` (12:05 AM, BT counter, red battery), `w16-clock-no-time.png`; inside the 110 px corner curve |
| 43 | BOOT steps the mode | pass | HW-LOG: `watch: BOOT, mode after WIFI` → `cmd: 'bt on'` → `bluetooth scan started`; `after BT` → `cap on` → `capture started`; `after CAP` → `cap off` → `stream started`. HW-EYE: a press with the screen dark woke it and moved the mode; the counter followed the mode. A first run logged six events in 50 s (two pairs 0.2–0.5 s apart) for what was meant to be two presses; the user had misread the instruction and pressed more often, so that count means nothing |
| 43a | One press = one step | pass | HW-LOG, throwaway build logging every level change of GPIO 0 (then removed): two single presses, each exactly one low edge (13130→13280 ms and 33020→33190 ms, 150–170 ms) and one `watch: BOOT` ~30–50 ms in. No chatter seen on the pin. Two presses, so a bounce that shows once in 100 would be missed |
| 44 | Four pages | pass | HW-EYE: swipes go clock → dashboard → networks → controls, dots centered |
| 45 | Builds | pass | BUILD: esp32s3 0x14d9a0 (30 % free); esp32c3 and esp32c6 unchanged; esp32c5 (CSI line removed) +0x10; warnings identical to untouched upstream |

## WardriveESP 1.7.6 (2026-11-03)

Upstream 1.7.6 (`rocketgod.dev/WardriveESP/WardriveESP.zip`, `FW_VERSION "1.7.6"`) committed pristine on the 1.7.5
vendor commit (branch `vendor/wardriveesp`, `d633973`) and merged into `main`. Upstream changed 11 files, PROTOCOL.md
none. Two files conflicted, both resolved toward upstream's intent:

- `lcd_draw.h`: upstream's 8-row strips (`LCD_SH 8`, half the DMA buffer) with the watch's 410×502 sizes.
- `lcd.c`: upstream dropped the idle backlight dim ("an offline board is still working, and the screen is how you
  tell"). The watch keeps its own brightness setting and screen-off timeout; it no longer dims after 30 s unlinked.

The watch's own "this session" count (`aps_link`) was replaced by upstream's new per-run count (`aps_run`, `apc_run()`):
the dashboard tile reads SENT while linked, SEEN otherwise, upstream's wording.

| # | Check | Result | Evidence |
|---|---|---|---|
| 46 | Upstream screens exactly as upstream draws them | pass | AUTO: all 28 240×320 / 128×128 scenes from the merged tree byte-identical to pristine 1.7.6's own `tools/lcd_preview.c` |
| 47 | Watch screens unchanged by 8-row strips | pass | AUTO: 13 watch scenes byte-identical before and after the merge; `w2`/`w7` differ only by the SENT/SEEN tile (viewed); `w9-alert` differs by at most 9/255 per channel in its full-screen gradient (built per strip from RGB565 endpoints, so its rounding follows strip height; not visible) |
| 48 | All chips build | pass | BUILD: esp32s3 0x14e370 (30 % free); esp32c3 0x1150f0, esp32c6 0x165960, esp32c5 (CSI line removed) 0x197860; warnings identical to upstream |
| 49 | Nothing of the watch leaks onto other chips | pass, with a known 48 bytes | BUILD: pristine 1.7.6 esp32c3 0x115080, esp32c6 0x1658f0; ours +0x70 on both. `nm` symbol diff (C3): only `BOARDS` (+16, the Waveshare entry in upstream's shared board table), `ext_build` (+16, the advert's screen flag names the watch) and `lcd_size` (+16, the 410x502 case), all from slice 1. On 1.7.5 the same additions fell inside alignment padding, so "C3 unchanged" (#21) was luck, not proof |
| 50 | 1.7.6 on the watch | pass | HW-LOG: `WD:BOOT fw=1.7.6`, `board: Waveshare AMOLED 2.06`, `clock set from rtc`, `touch FT3x68: ready`, `WardriveESP 1.7.6 ready`; phone linked and streamed; BOOT → `bt on` → `bluetooth scan started`, → `cap on` → `capture started`; pages 0–3. HW-EYE: clock right, BOOT stepped the mode, SENT tile while linked; Wardrive Go shows 1.7.6 |
| 51 | Frame cost with 8-row strips | pass | HW-LOG: `lcd: 68 frames in 60 s, avg 122 ms (paint 116, panel 11, touch 3, state 3), worst 576 ms, … stack left 1108`, capture running. One sample; not slower than 16-row strips (#37: avg 164 ms while capturing) |

## Charging indicator (2026-11-03)

Registers from XPowersLib `XPowersAXP2101.tpp` in the Waveshare repo at `30f3b23`: STATUS1 `0x00` bit 5 USB power
good (`isVbusGood`); STATUS2 `0x01` bits 7:5 battery current, `01` charging (`isCharging`), bits 2:0 charger state
(`getChargerStatus`). Read every 2 s on the health task (the level stays every 30 s); each change is logged.

| # | Check | Result | Evidence |
|---|---|---|---|
| 52 | Charging is read | pass | HW-LOG at boot on USB: `watch: charging (power status 38 32)`, `battery AXP2101: 31%, 3929 mV, charging`. `38`: USB good, battery present; `32`: charging, constant-current phase |
| 53 | Plug and unplug | pass | HW-EYE: bolt shown while charging, gone on unplug, back on replug (the serial log can't follow an unplug: USB is the port) |
| 54 | Drawing | pass | AUTO/visual: `docs/previews/w17-clock-charging.png` (green bolt, green percent), `w18-dashboard-charging.png` (white bolt over the header's cell), `w19-dashboard-usb-full.png` and `w20-clock-usb-full.png` (dim bolt). BUILD: esp32s3 0x14e5d0; esp32c3/c6/c5 unchanged. Full state seen later: HW-LOG `on USB power, not charging (power status 28 14)` at 100 %, HW-EYE "all normal" (#59) |

## Memory and stability (2026-11-03)

Full records: `docs/memory-audit.md` (architecture, allocations, PSRAM and partition decisions), `docs/memory-baseline.md`
(measured heap and stacks), `docs/memory-soak-results.md` (stress runs, thresholds, rendering).

| # | Check | Result | Evidence |
|---|---|---|---|
| 55 | Screen task stack | pass after a fix | HW-LOG: 1,060 of 4,096 left when a MODE/BOOT press ran the role switch on `wd_lcd`; after moving it to `wd_health` (`ea697af`) the lowest seen is 1,604 (settings holds), 1,748 with presses, alerts and the log line |
| 56 | Stress on hardware | pass | HW-LOG: 102 mode switches, 50 alerts, 20 of 20 BLE reconnects, 10 min of touch; 0 resets, 0 allocation failures, 0 task watchdog events, 0 `txdrop`; per-minute heap minimum within a 2 KB band for 67 min; lowest free heap 62,008, smallest largest block 31,744 |
| 57 | Rounded-rectangle coverage decided per pixel first (`abfe4d9`) | pass | AUTO: 76 preview scenes byte-identical. HW-LOG: alert paint 286 → 162 ms, worst frame 461 → 244 ms; dashboard paint 186 → 156 ms |
| 58 | `lcd_draw.c` at `-O2`, S3 only (`a2c0fc3`) | pass | AUTO: 76 scenes byte-identical at -Og and -O2. HW-LOG: alert paint 162 → 117 ms, frame 198 → 148 ms; `wd_lcd` 1,856–1,904 left |
| 59 | Regression, default image | pass | HW-LOG: `fw=1.7.6`, clock from the chip, battery, touch, phone link and Wi-Fi stream, BOOT ×3 through Bluetooth and capture, screen off and on; no `WD:` change since `a406ec3`. HW-EYE: "all normal" |
| 60 | Thirty-minute soaks (Wi-Fi display on/off, Bluetooth, capture) | NOT RUN | scheduled; procedure in `memory-soak-results.md` section 3 |

## Web flasher and releases (2026-11-03)

| # | Check | Result | Evidence |
|---|---|---|---|
| 61 | A clean build is the tested build | pass | BUILD: `set-target esp32s3` from `sdkconfig.defaults` in a fresh tree gives an `sdkconfig` identical to `build-esp32s3`'s and the same image size (0x1502E0) |
| 62 | The release's merged image boots | pass | HW-LOG: `write_flash 0x0` of the 1,508,064-byte merged image, then `fw=1.7.6`, board, clock from the chip, battery, touch, phone linked. Also seen: `boots=1 prev_fw=?`, because the merged image overwrote NVS (documented) |
| 63 | Install from the web flasher, then pair | pass | HW-EYE: the user installed through the flasher (served locally, `http://127.0.0.1:8642`) in their browser with **Erase device** ticked, and Wardrive Go linked. HW-LOG after the browser released the port: `fw=1.7.6`, clock from the chip, `phone connected`, `notifications on`, `stream started`. Page check (headless Chromium): ESP Web Tools loaded, Web Serial available, manifest read, no console errors |
| 64 | Release workflow | partly checked | AUTO: `actionlint` clean; the Version (good tag, mismatched tag, branch), Package and release-notes steps run locally against the clean build. NOT RUN on GitHub yet: needs Pages enabled and a tag |
| 65 | The deployed site (GitHub Pages, https) | NOT RUN | after the first release |
