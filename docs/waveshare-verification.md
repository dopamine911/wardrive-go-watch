# Waveshare watch port: verification record

Every result is labelled with the kind of evidence behind it:

| Label | Meaning |
|---|---|
| **BUILD** | compiler output |
| **HW-LOG** | the watch's serial log or USB protocol output |
| **HW-EYE** | the user looking at the watch or the phone |
| **INFER** | reasoning, not observed |

Nothing here comes from a simulator, and there are no automated tests yet.

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
- **Wardrive Go sends alerts to the watch.** HW-LOG: `cmd: 'led alert spin FFD23B 6 TRACKER'`. On S3 `cmd_led` is
  still the upstream stub, so the watch replies "no LED ring" and shows no banner. This settles analysis R3.
- **Opening the serial port resets the watch** unless RTS drops before DTR. A reset drops the phone link. Use
  `idf.py monitor --no-reset` (checked: uptime 495 s → 505 s with no reset).

### Open issues

| # | Issue | Status | Evidence |
|---|---|---|---|
| O1 | One screen freeze plus the watch dropping off USB | **not reproduced** | HW-EYE: dashboard frozen at clock 0:04 (about 6 s after boot) and the watch absent from `lsusb` while plugged in. It recovered after the app was foregrounded and the cable replugged. This happened right after a log capture that reset the watch and then closed the port. The freeze did not recur with the app backgrounded for 60 s (#11). The kernel log is unreadable (user not in `adm`). INFER: tied to the bench reset or port close, not to the firmware's normal running. Not proven. |
| O2 | Screen setup holds up boot for about 1.4 s before Wi-Fi starts | known | HW-LOG: `board:` at 564 ms, `lcd … running` at 1934 ms. Same synchronous pattern as the upstream color screens |
| O3 | Dashboard is the 240×320 layout, centred | by design for slice 1 | The native 410×502 layout is Phase 4 |
| O4 | Battery, touch, phone alerts on screen, screen sleep, brightness controls | not started | Phases 2–4 |
| O5 | Sustained soak (≥30 min concurrent scan and render) | not run | acceptance check 6 in the analysis |
| O6 | esp32c5 can't be built with the local IDF | upstream/toolchain | #3; needs the IDF release upstream uses |
