# Memory baseline: Waveshare watch, WardriveESP 1.7.6 + watch port

Measured 2026-11-03 on the watch (ESP32-S3 rev v0.2, `/dev/ttyACM0`), linked to Wardrive Go (Android, app in the
foreground) unless stated. Image: the bench build `build-esp32s3-diag` (`CONFIG_WARDRIVEESP_MEMDIAG=y`) of `5c0a5aa`
and, from the row marked "fix", `ea697af`. All figures HW-LOG from `MD` lines (section 9 of `memory-audit.md`) or
`WD:HEALTH`, unless labelled otherwise. Stack figures are bytes never used (high-water marks), lowest seen.

## How to reproduce

```sh
idf.py -B build-esp32s3-diag -D SDKCONFIG=build-esp32s3-diag/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.memdiag" set-target esp32s3 build flash
```

Then, on the USB port without opening a session (Bluetooth stays up): `diag` prints a snapshot now; an `MD seq=N ...`
line arrives every 60 s; `diag dump` replays the last 96 from RTC memory (they survive a software reset); `diag clear`
starts over. `bench wake|mode|alert|drop|lcdlog 0|1` drive the stress scenarios (`memory-soak-results.md`). Any
serial tool that drops RTS before DTR works (opening the port otherwise resets the watch).

Diagnostics overhead: +4.3 KB of code (0x151390 vs 0x1502E0 at `a2c0fc3`); the 7.3 KB ring is in RTC slow memory, not the heap;
1 KB of static line buffers. Sampling runs once a second on `wd_health` (a free-heap read, a largest-block walk, one
queue count); the line goes through the USB driver with a 20 ms cap.

## Heap (internal; this image has no PSRAM)

| State | Free | Lowest in the minute | Largest free block | Note |
|---|---|---|---|---|
| Boot, phone linked, Wi-Fi scan, no other mode visited yet | 84,036 – 84,052 | 82,588 – 83,144 | 38,912 | `heap_caps_get_total_size(INTERNAL)` = 273,212 |
| Same image, USB session (Bluetooth off) | 147,048 – 147,056 | — | 38,912 | `WD:HEALTH`; Bluetooth costs ≈ 63 KB |
| Phone linked, Wi-Fi, after capture and Bluetooth modes were visited | 72,124 – 73,040 | 71,308 – 72,816 | 31,744 | the capture queue (8.3 KB) and Bluetooth record queue (2.1 KB) stay allocated while linked (audit R5, R6): held for the next use, not leaked |
| Phone linked, Bluetooth mode | 77,140 – 77,252 | 71,868 – 72,020 | 31,744 | `wd_bt` 6 KB instead of `wd_scan` 10 KB |
| Phone linked, capture mode | 73,028 – 73,040 | 71,324 – 71,976 | 31,744 | |
| **Lowest since boot (allocator record)** | | **62,008 – 62,136** | | at a capture start: queue and 10 KB stack before the previous task's stack is freed |
| **Smallest largest block** | | | **31,744** | the largest single request is a 10 KB task stack |

The figure in verification #12 (heap 160,564) was a USB-session reading on the slice-1 image: Bluetooth was off.
It is not comparable with phone operation.

## Task stacks (bytes never used)

| Task | Size | Lowest left | Under |
|---|---|---|---|
| `wd_lcd` | 4,096 | **1,060 before the fix**, **1,604 after** (`ea697af`) | before: a MODE/BOOT press ran the role switch on this task; after: 1,748 with swipes, alerts, mode presses and the `lcd:` line; 1,604 with BRIGHTNESS / SCREEN OFF holds (they save to NVS from this task) |
| `wd_health` | 6,144 | 2,576 – 2,716 | runs the phone's commands and (after the fix) the watch's |
| `wd_usb` | 8,192 | 4,820 | `bench` commands run `run_command` here, as USB-session commands do |
| `wd_scan` | 10,240 | 7,176 – 7,180 | |
| `wd_bt` | 6,144 | 3,284 – 3,364 | |
| `wd_cap` | 10,240 | 6,672 – 6,880 | |
| `wd_btpwr` | 4,096 | 3,324 | |
| `nimble_host` | 6,144 | 2,636 – 2,652 | |
| `wifi` (IDF) | (not configurable here) | 4,320 | |
| `esp_timer` | 3,584 | 2,852 – 3,012 | |
| `wd_gps` | — | exits at boot | HW-LOG `GPS: GPIO 4 belongs to this board's own hardware, GPS off` |

### Where `wd_lcd`'s stack goes (Phase 4)

The high-water mark only falls, so the steps ran in order of suspicion from a fresh boot, with the `lcd:` diagnostic
line switched off (`bench lcdlog 0`) until the last step:

| Step | `wd_lcd` left |
|---|---|
| Rendering the clock face, 2 min | 2,020 |
| Swipes through all four pages (each logs `watch: page N` from this task) | 1,780 |
| Three alerts | 1,780 |
| Three BOOT presses (`watch_scan` → `run_command` → role switch, on this task) | **1,060** |
| `lcd:` log line back on, two lines | 1,060 (no deeper) |

Static frames (`-fstack-usage`, BUILD): the largest on the drawing path is `w_networks` 208 B; `text_ex` 96 B. The
depth came from the command, not the drawing. The fix moved the command to `wd_health` (`ea697af`); repeated with the
log line on from boot, swipes, alerts and BOOT/MODE presses: **1,748 left**. No stack increase was needed.

## BLE output, queues, faults

| Metric | Value |
|---|---|
| msys output buffers | 36; fewest free in any minute: 35 |
| `txdrop` (frames dropped) | 0 in every minute |
| Bluetooth/capture queue drops | 0 in every minute |
| Allocation failures | 0 |
| Task watchdog events | 0 |
| Resets during measurement | 0 (only the deliberate ones at test start) |

## Drawing cost on the chip (`-Og`, as built until `abfe4d9`)

| Scene | Frame avg | Paint | Panel | Worst |
|---|---|---|---|---|
| Clock face, little changing | 59 – 60 ms | 54 | 0 | 81 – 97 |
| Dashboard and pages, phone linked | 103 – 173 ms | 122 – 154 | 7 – 12 | 255 – 575 (boot minute) |
| Alert (repeated) | 316 ms | 286 | 19 | 461 |

Touch polling costs 1–6 ms a frame, gathering state 3 ms. Painting is the cost. After the two drawing changes
(`memory-soak-results.md` section 4): dashboard paint 186 → 156 ms (`rr_cov`), alert paint 286 → 162 → 117 ms
(`rr_cov`, then `-O2`), alert worst frame 461 → 211 ms.
