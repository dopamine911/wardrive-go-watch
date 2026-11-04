# Memory audit: Waveshare ESP32-S3 Touch AMOLED 2.06 (WardriveESP 1.7.6 + watch port)

Date: 2026-11-03. Source: `main` at `a406ec3`. Build: ESP-IDF v5.5.2, `build-esp32s3/sdkconfig`.
Evidence labels: BUILD, AUTO, HW-LOG, HW-EYE, INFER (as in `waveshare-verification.md`). Runtime figures are not in
this file: they belong in `memory-baseline.md`, measured with the instrumentation described in section 9.

## 0. Corrections to the task's premises

| Premise | Actual | Evidence |
|---|---|---|
| "WardriveESP 1.7.5-based fork" | 1.7.6 since `9eb5e19` | `FW_VERSION "1.7.6"`; HW-LOG `WD:BOOT fw=1.7.6` (verification #50) |
| Firmware binary 0x14D9A0 | 0x14E5D0 (1,369,552 B) | BUILD, `a406ec3` |
| "Appears to use a 4 MB flash layout without PSRAM" | Confirmed: `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`, no `CONFIG_SPIRAM*` at all | BUILD (`sdkconfig`) |
| Baseline heap 160,564 / min 95,068 / largest 53,248 / scan stack 7,180 | One `WD:HEALTH` sample from slice 1 (`61d88b7`, 1.7.5, no watch pages yet) | verification #12. Not comparable to today's image |
| Display stack ~1,024 B left | Two samples: 1024 (`06f0de2`), 1108 (`9eb5e19`, 8-row strips) | verification O12, #51. Measured by the `lcd:` diagnostic line itself (section 5) |

## 1. Build configuration that shapes memory

| Setting | Value | Consequence |
|---|---|---|
| Flash size | 4 MB (chip has 32 MB) | Section 8 |
| PSRAM | not enabled | Every heap allocation is internal SRAM |
| Optimization | `-Og` (`COMPILER_OPTIMIZATION_DEBUG`, upstream's choice for all chips) | Larger, slower code than `-O2`; relevant to rendering (section 6) |
| Stack overflow check | canary only (`FREERTOS_CHECK_STACKOVERFLOW_CANARY`) | Catches an overrun only at a context switch, after the damage |
| Task watchdog | 5 s, idle tasks of both cores, **no panic** (`ESP_TASK_WDT_PANIC` unset) | A starved core logs a warning and carries on: watchdog events must be counted, a reset count won't show them |
| Core dump | none | Resets are diagnosed from upstream's own breadcrumbs (`s_crash` in `.rtc_noinit`, `WD:BOOT reset=`, `WD:CRASH`) |
| Heap poisoning / tracing | off / off | No per-allocation tracking in normal builds |
| FreeRTOS trace facility | off | No `uxTaskGetSystemState`; stacks are read per task handle |
| Console | UART0 primary, USB-Serial-JTAG secondary; upstream also installs the USJ driver for its USB link | Two writers to one USB FIFO: a candidate cause of the log loss in O10 [INFER] |
| NimBLE | 1 connection, host task 6144 B, internal allocation, msys 12×256 + 24×320, ACL 24×255, events 30×70 | ≈19 KB of Bluetooth buffers while Bluetooth is up [INFER from config, not measured] |
| Wi-Fi | static RX 10, dynamic RX 32, dynamic TX 32, mgmt 32, AMPDU RX/TX window 6 | Dynamic buffers come and go with traffic: the main source of heap churn outside our code [INFER] |

## 2. Memory map (static, BUILD: `idf.py size`)

| Region | Used | Free | Total |
|---|---|---|---|
| DIRAM `.text` (IRAM code placed in DIRAM) | 117,923 | | |
| DIRAM `.bss` | 47,424 | | |
| DIRAM `.data` | 22,104 | | |
| **DIRAM total** | **187,451** | **154,309** | 341,760 |
| IRAM | 16,384 | 0 | 16,384 |
| RTC slow | 124 (92 `.rtc_noinit` crash breadcrumbs) | 8,068 | 8,192 |
| RTC fast | 24 | 8,168 | 8,192 |
| Flash code / rodata | 848,586 / 364,408 | | |

The 154 KB left in DIRAM is the heap at boot, before Wi-Fi, Bluetooth, tasks and queues take their share.

Static RAM by archive (BUILD, `esp_idf_size --archives`): `libmain.a` (this firmware) 26,619; `libnet80211` 8,538;
`libbt` 5,278; `libpp` 3,874; `libfreertos` 3,863; `libhal` 3,484; `libphy` 3,358; `liblwip` 2,482.

### This firmware's largest statics (BUILD, `nm` per object)

| Symbol | Bytes | Owner | Note |
|---|---|---|---|
| `s_cap_aps_tbl` | 7,680 | upstream | capture AP cache, 160 entries; resident on every chip even when capture never runs |
| `s_cap_seen` | 2,048 | upstream | capture dedup fingerprints |
| `s_frame` (lcd.c) | 1,976 | watch | one frame's state; 1,664 of it is the 32-row sighting list |
| `s_netlog`, `s_btlog`, `nets` | 1,668 each | watch | Wi-Fi list, Bluetooth list, the screen task's copy |
| `s_cmd_ring` | 1,536 | upstream | 6 BLE commands × 256 |
| `l_reg`, `s_reg` (apcount) | 1,536, 1,024 | upstream | HyperLogLog sketches |
| `chunk` (ota_task) | 1,024 | upstream | OTA receive chunk, static |
| `s_cap_m1` | 960 | upstream | |
| `buf` | 512 | upstream | |
| `s_line` | 384 | upstream | output line |

Watch-specific statics total ≈6.7 KB (the three lists, the frame's copy of one, plus small state). [BUILD]

## 3. Tasks

Stacks are bytes (ESP-IDF FreeRTOS). No task here is pinned unless noted; `xTaskCreate` = either core.

| Task | Stack | Prio | Core | Lifetime on the watch | Source |
|---|---|---|---|---|---|
| `main` | 3,584 | 1 | 0 | boot only (returns after `ready`) | sdkconfig |
| `wd_usb` | 8,192 | 4 | any | always | `usb_link_start` |
| `wd_health` | 6,144 | 3 | any | always; also runs BLE commands since 1.7.6 | `app_main` |
| `wd_btpwr` | 4,096 | 4 | any | always | `app_main` |
| `wd_gps` | 6,144 | 4 | any | **deletes itself at boot on the watch** (GPIO 4 is a watch pin) [INFER from code; boot log to confirm] | `gps_task` |
| `wd_lcd` | 4,096 | 1 | any | always (screen) | `lcd_start` |
| `wd_scan` | 10,240 | 5 | any | Wi-Fi mode while a host listens | `wifi_start` |
| `wd_bt` | 6,144 | 5 | any | Bluetooth mode | `bt_start` |
| `wd_cap` | 10,240 (7,168 fallback on RISC-V only) | 5 | any | capture mode | `cap_start` |
| `wd_csi` | 6,144 | 5 | any | CSI mode (S3 has no CSI in this build: `csi=0`) | |
| `ota` | 6,144 | 4 | any | during an update | |
| `reboot` | 2,048 | 5 | any | at reboot | |
| `wd_sd`, `wd_led`, `wd_disp` | 4,096 / 4,096 / 3,072 | | | never on the watch (no card slot, ring, OLED) | |
| `nimble_host` | 6,144 | (IDF) | 0 | while Bluetooth is up (off during a USB-only session) | sdkconfig |
| `btController`, `wifi`, `esp_timer` (3,584), `ipc0/1` (1,280), `IDLE0/1` (1,536), `Tmr Svc` (2,048), `sys_evt` (2,304), `tiT` (3,072) | | | | IDF; sizes from sdkconfig where given; `wifi` and `btController` not configurable here | |

Steady state on the watch in Wi-Fi mode, our tasks only: 8,192 + 6,144 + 4,096 + 4,096 + 10,240 = 32,768 B of stack
(+6,144 GPS if it does not exit). Bluetooth mode swaps 10,240 for 6,144; capture keeps 10,240.

Existing stack reporting: `WD:HEALTH` (every 15 s while a host listens) carries `stack_scan/bt/cap/gps/usb/health`
and `stack_csi`. Not covered: `wd_lcd` (only in its own `lcd:` log line), `wd_btpwr`, `nimble_host` and all IDF tasks.

## 4. Dynamic allocations and lifetimes

| What | Size | When | Freed | Risk |
|---|---|---|---|---|
| LCD strip (DMA, internal) | 410 × 8 × 2 = 6,560 | `lcd_start` | never | none (one block at boot) |
| QSPI bus (`max_transfer_sz` 6,560), panel IO queue depth 2 | driver-sized | boot | never | none |
| USB-Serial-JTAG driver | TX 4,096 + RX 1,024 | boot | never | none |
| `wifi_ap_record_t` array per channel scan | ≤ 60 × 80 ≈ 4.8 KB | each channel dwell | at once | churn: allocate/free several times a second in Wi-Fi mode |
| Bluetooth record queue | 48 × 44 ≈ 2.1 KB (half on failure) | first `bt on` | **never** (kept for the next `bt on`) | held, not leaked |
| Capture queue | 32 × 260 ≈ 8.3 KB (½, ¼ fallbacks) | `cap on` | when no host is linked (`free_idle_queues`) | held while linked after leaving capture |
| Capture/BT/scan task stacks | 10,240 / 6,144 / 10,240 | mode start | mode end | the largest single blocks requested at run time: they need a contiguous block, so fragmentation shows up here first |
| `s_sd_line` | 1,184 | an `sd` command | after it | none on the watch (no slot) |
| OTA stream buffer | 24 KB, falling back to 16/10/6 KB | update | after | an update while scanning competes for the largest block |
| NimBLE msys/ACL/event pools | ≈19 KB [INFER] | Bluetooth up | Bluetooth down (USB-only session) | large swing between USB and phone operation |
| Wi-Fi dynamic RX/TX | up to 32 × ~1.6 KB each way [INFER, driver] | per packet | per packet | churn; peak demand under heavy traffic |
| NVS, FATFS LFN (`FATFS_LFN_HEAP`) | small | NVS writes; card never on the watch | | none |

Unbounded growth: none found. Every list in our code is fixed-size (`netlog` 32, capture tables, apcount
sketches); queues are fixed-length; `WD:` output blocks or drops (`txdrop`), it never queues without bound. [BUILD:
source read]

## 5. Display and fonts

- Fonts: flash `.rodata` (`lcd_fonts.h`, `lcd_fonts_watch.h`, ≈114 KB for the watch set). Read through the flash
  cache on every glyph: a candidate for cache misses during painting (section 6) [INFER].
- Strip hashes: `s_hash`, 63 × 4 B.
- Frame state `s_frame` 1,976 B static; the 32-entry list copy is made under a spinlock on the screen task.
- Stack measurement caveat: the `lcd:` line reports `uxTaskGetStackHighWaterMark` from inside the `ESP_LOGI` that prints
  it, after a minute that included that same call. The deepest point may be the logging call itself (O12). Phase 4
  separates the two.

## 6. Bluetooth output path

- Output frames come from NimBLE's msys pool (12 + 24 blocks). Since 1.7.6 senders keep `TX_RESERVE` (4) free, the
  stream keeps `REPLY_ROOM` (6) more, and non-host tasks wait up to 100 ms (`tx_wait`) before dropping
  (`s_tx_dropped`, `txdrop=`).
- Commands from the phone: a 6-slot ring, run on `wd_health`.
- The watch's MODE/BOOT run commands from `wd_lcd` through `run_command` (`s_cmd_lock`), so a busy link can hold the
  screen task for up to ~100 ms per output line [INFER from `tx_wait`].

## 7. Internal SRAM versus PSRAM

All allocations are internal today. Ones that must stay internal if PSRAM were enabled: the LCD strip (DMA),
SPI/QSPI and USB driver buffers, task stacks that touch flash-disabling paths, ISR data, and Wi-Fi/Bluetooth
controller memory unless the IDF options for it are used. Candidates that could move: the capture AP table
(7,680), capture queue (8.3 KB), the sighting lists (5 KB) and the frame copy. Section 10 risk R2 covers why
enabling PSRAM is not free on this shared image.

## 8. Partitions and OTA

| Partition | Offset | Size |
|---|---|---|
| nvs | 0x9000 | 0x5000 |
| otadata | 0xE000 | 0x2000 |
| phy_init | 0x10000 | 0x1000 |
| ota_0 | 0x20000 | 0x1E0000 (1,966,080) |
| ota_1 | 0x200000 | 0x1E0000 |

Image 1,369,552 B = 69.7 % of a slot; 596,528 B headroom. The table is upstream's, shared by every S3 board; the
rocketgod.dev flasher writes a merged image with `eraseAll` (analysis finding 9), which puts this table back.

## 9. Instrumentation plan (Phase 2)

Constraints: no new `WD:` keys (D3), nothing that blocks the scan or BLE paths, off in published builds.

1. **Kconfig `WARDRIVEESP_MEMDIAG`** (default n), built as a separate bench image
   (`build-esp32s3-diag`, `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.memdiag"`). Default builds stay identical.
2. **Sampler on `wd_health`** (already wakes every second): each second it folds the free internal heap, the
   largest internal block and the free 8-bit heap into per-interval minima; every interval (default 60 s) it records
   one snapshot.
3. **Snapshot**: uptime, interval minima and lifetime minimum of free internal heap, largest block, stack high-water
   marks of `wd_lcd`, `wd_scan`/`wd_bt`/`wd_cap`, `wd_usb`, `wd_health`, `wd_btpwr`, `nimble_host`, `wifi`,
   `esp_timer` (by handle or `xTaskGetHandle`), msys free blocks (min in interval), `txdrop`, Bluetooth and capture
   queue depth and drops, APs counted and sweeps, screen frames / average / worst ms, allocation failures, task
   watchdog events, role and link bits.
4. **Ring in RTC slow memory** (`RTC_NOINIT_ATTR`, ≈5.6 KB of the 8 KB free): survives a software reset or panic, so
   the minutes before an unexpected reset can be read after it; costs no heap. Each entry carries a sequence number
   and a checksum, so a dump that loses lines (O10) shows it.
5. **Counters**: `heap_caps_register_failed_alloc_callback` (failures, last size and caps); the task watchdog's
   `esp_task_wdt_isr_user_handler` (events).
6. **Output**: one compact `diag` log line per interval, and bench commands on the USB port **without** opening a
   session (a session turns Bluetooth off, which would change what is being measured): `diag` (now), `diag dump`
   (the ring), `diag clear`. For stress scenarios that otherwise need the app's cooperation: `bench mode` (the same
   path as the MODE button), `bench alert` (the same `lcd_alert` the app's alerts use), `bench drop` (end the BLE link
   so the phone reconnects).
7. **Display stack (Phase 4)**: snapshots read `wd_lcd`'s high-water mark from `wd_health` by handle, so the reading
   no longer depends on the screen task's own log call; a run with the `lcd:` line suppressed shows whether that call
   is the deepest point. `-fstack-usage` on `lcd.c`/`lcd_draw.c` gives per-function frames (BUILD).
8. **Rendering (Phase 6)**: the existing per-frame split (paint, panel, touch, state) plus, in the diag build, cycle
   counts for glyph drawing versus fills, and an A/B of `lcd_draw.c` at `-O2`.
9. **Overhead**: compare free heap, image size and frame time between the diag and default images.

## 10. Risks found by reading (to be confirmed or cleared by measurement)

| # | Risk | Why | How it is checked |
|---|---|---|---|
| R1 | Fragmentation stops a mode switch | Mode starts need 6–10 KB contiguous stacks plus queues while Wi-Fi's dynamic buffers and the per-channel scan array churn the heap | Largest-block minima across 100 mode transitions; `WD:ERR reason=no_task/no_memory`; 1.7.6's fallback-to-Wi-Fi log line |
| R2 | PSRAM on the shared S3 image | The watch's PSRAM is octal (Waveshare README: "OPI PSRAM"); octal mode claims GPIO 33–37 on every S3 board the stock image runs on | Section 7; Phase 5 decides with numbers |
| R3 | Display stack headroom | 1,024–1,108 B left of 4,096 | Phase 4 |
| R4 | Silent task-watchdog events | No panic configured | Counter (section 9.5) |
| R5 | Capture queue held after capture while linked | 8.3 KB kept until the host unlinks | Heap before/after `cap on` → `cap off` while linked |
| R6 | Bluetooth record queue never freed | 2.1 KB after the first `bt on` | Same |
| R7 | Log loss in long captures (O10) | Two writers on the USB FIFO [INFER] | Sequence-numbered `diag` lines show gaps; the RTC ring is the fallback |
| R8 | Screen task blocked by BLE backpressure | MODE/BOOT send lines from `wd_lcd`; `tx_wait` up to 100 ms each | Worst frame time around mode presses |

## 11. Test plan needing the hardware (Phase 3)

| Scenario | Duration | Host | Driven by | Needs from the user |
|---|---|---|---|---|
| Wi-Fi, display on | 30 min | phone (BLE) | touch every <30 s or screen-off set to 120 s and periodic touch | app in the foreground, phone awake |
| Wi-Fi, display asleep | 30 min | phone | none (screen times out) | same |
| Bluetooth discovery | 30 min | phone | `bench mode` / BOOT | same |
| Capture | 30 min | phone | `bench mode` / BOOT | same |
| Touch and swipe | 10 min | phone | the user | swiping |
| BLE disconnect/reconnect | 20 cycles | phone | `bench drop` | app in the foreground (it reconnects on its own [to verify]) |
| Alerts | 50 | phone | `bench alert` | none beyond the link |
| Mode switching | 100 transitions | phone | `bench mode` | none beyond the link |

The four 30-minute scenarios can run back to back in one ≈2 h session with the ring as the record (60 s interval ×
96 entries = 96 min, dumped twice).
