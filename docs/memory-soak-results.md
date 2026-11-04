# Sustained and stress testing: results

Watch on USB to the bench computer, linked to Wardrive Go (Android, app in the foreground, phone screen kept awake).
Bench diagnostics image (`memory-baseline.md`: how to build and read it). All rows HW-LOG unless labelled.
Images: G1, G2, G3, G3b and F ran on `ea697af` (button commands on `wd_health`); the rendering "after" rows on the
`rr_cov` change (section 4).

## 1. Thresholds and why

Derived from `memory-baseline.md`, not from general rules:

| Metric | Pass if | Reasoning |
|---|---|---|
| Resets, panics, stack-overflow reports | none | |
| Task watchdog events (`twdt`) | 0 | the watchdog doesn't reset this image, so the count is the only trace |
| Allocation failures (`allocfail`) | 0 | every failure was 0 in all measurement so far |
| Lowest free internal heap since boot | ≥ 55,000 | measured worst 62,008 (a capture start); 7 KB below it is one more capture queue: a new peak demand worth finding |
| Smallest largest free block | ≥ 24,576 | measured 31,744; 24 KB keeps OTA's full-size buffer (it falls back to 16/10/6 KB below that) and three 10 KB task stacks' worth of room |
| Heap trend in a steady state (same mode, same link state) | median of the per-minute minimum (`heap_min_iv`) in the last 5 minutes within 2 KB of the first 5 | the per-minute minimum moves within ≈2 KB at rest (70,924 – 72,816 over 67 min of G runs) |
| Any task's stack left | ≥ 768 | upstream's own `low_stack` mark in `WD:HEALTH` |
| `txdrop`, queue drops | not growing minute after minute in a steady state | |
| Scanning while the display sleeps | `sweeps` (Wi-Fi) or `aps` keep rising in minutes flagged dark | acceptance: display sleep must not interrupt scanning |
| BLE link | no disconnect except the ones a test causes | |

## 2. Results

| Scenario | Required | Run | Result | Key numbers |
|---|---|---|---|---|
| Mode switching (G1) | 100 transitions | 102 (`bench mode`, every 6 s) | **pass** | 34 each of `bt on`/`cap on`/`cap off`, each followed by its start line; 0 `couldn't start`, 0 `WD:ERR`; heap per-minute min 71,324 – 72,816, no trend; largest block min 31,744; `wd_lcd` 1,748 |
| Alerts (G2) | 50 | 50 (`bench alert`, every 4 s, 2 s each) | **pass** | 50 run, 50 acknowledged; heap per-minute min 71,308 – 71,956; `wd_lcd` 1,748 |
| BLE disconnect/reconnect, first attempt (G3) | 20 cycles | 20 drops, 11 real | invalid test, no fault | the app's reconnect backs off to ≈61 s, so drops every 30 s hit an unlinked board; every reconnect completed |
| BLE disconnect/reconnect (G3b) | 20 cycles | **20 of 20** | **pass** | drop → `notifications on` 1.1, 1.1, 11.4, 21.4, 41.0, then 61.0 – 62.0 s; linked heap 71,592 – 72,804 before and after (no growth per cycle); unlinked 83,504 – 83,508 |
| Touch and swipe (F) | 10 min | 10 min | **pass** | 224 page swipes, 18 brightness and 13 screen-off holds; heap per-minute min 70,952 – 71,896; `wd_lcd` new low **1,604** (the settings save to NVS on this task) |
| Wi-Fi scanning, display active | 30 min | — | **NOT RUN** (scheduled with the user) | |
| Wi-Fi scanning, display asleep | 30 min | — | **NOT RUN** | |
| Bluetooth discovery | 30 min | — | **NOT RUN** | |
| Capture | 30 min | — | **NOT RUN** | |

Across G1–G3b and F (67 minutes of uptime, MD sequence 1 → 66 with no gap): resets 0, `twdt` 0, `allocfail` 0,
`txdrop` 0, queue drops 0, fewest free BLE output buffers 34 of 36, lowest free heap 62,032, smallest largest block
31,744, every stack above 1,600.

App behaviour seen in G3/G3b [INFER: not visible in the firmware, which accepted every attempt at once]: after
repeated drops Wardrive Go waits ≈61 s before reconnecting, and holding the link 30 s does not reset that.

## 3. How to run the 30-minute soaks

Flash the diagnostics image (`memory-baseline.md`), open the app (foreground, phone awake), then on the bench computer
with `firmware/tools/bench_serial.py` (timed lines, no session; e.g. `python tools/bench_serial.py /dev/ttyACM0 log
1830 1:"diag clear" 30:"bench wake" 60:"bench wake" ...`):

1. `diag clear`.
2. Wi-Fi, display active: `bench wake` every 30 s for 30 min.
3. Wi-Fi, display asleep: nothing for 30 min (the screen times out; check `flags` has 8 in those minutes).
4. Bluetooth: `bench mode` once, 30 min.
5. Capture: `bench mode` once, 30 min.
6. `diag dump` (the ring holds 96 minutes; dump after step 3 as well).

Reconnect cycles: `python tools/bench_reconnect.py /dev/ttyACM0 20 30 2700`. Judge each step against section 1; first
and last 5 minutes compared within the same step.

## 4. Rendering (Phase 6)

### Where the time goes

On the chip (`lcd:` line, HW-LOG), per frame: painting 54–286 ms, panel transfer 0–28 ms, touch polling 1–6 ms,
gathering state 3 ms. Logging is once a minute. Painting is the cost.

Inside painting, the host profile of the same code (`tools/lcd_bench.c`, gprof, `-Og`, AUTO): `rr_cov` (rounded-
rectangle coverage, 16 samples per corner pixel) 58 %, colour `mix` 10 %, `fill` 9 %, `text_ex` 6.5 %, `blend` 4 %,
`rrect_line` 3.6 %, `circle` 3 %. Glyph drawing (`text_ex`, `glyph_of`, most of `blend`) is about 11 %: the fonts were
not the problem. Strip hashing (FNV over each strip, 102,910 32-bit words a frame) is inside the chip's "paint" figure
and was not timed on its own [INFER: a few cycles a word, ≈1–2 ms of a 117–186 ms paint].

### Changes, each measured on the watch

| Change | Scene | Paint | Frame | Worst frame |
|---|---|---|---|---|
| before (`ea697af`) | dashboard | 186 ms | 199 – 204 ms | 215 – 237 ms |
| `rr_cov` decides whole pixels first (`abfe4d9`; same pixels: 76 scenes byte-identical) | dashboard | 156 ms | 171 ms | 205 ms |
| before (`ea697af`) | alert, 50 in a row | 286 ms | 316 ms | 451 – 461 ms |
| `rr_cov` (`abfe4d9`) | alert | 162 – 163 ms | 198 – 200 ms | 234 – 244 ms |
| `lcd_draw.c` at `-O2`, S3 only (`a2c0fc3`; 76 scenes byte-identical at -Og and -O2 on the host) | alert | 117 – 118 ms | 148 ms | 211 – 214 ms |

Constraint: CPU work. The panel takes ≤ 28 ms; painting fell with every cut in instructions (−43 % algorithm, −28 %
compiler), and the chip gained more from `-O2` than the host did (−28 % against −13 %) [INFER: `-Og` code keeps values
on the stack]. Flash access to fonts is not the limit (glyphs are 11 % of the work).

Not reached: upstream's alert loop asks for a frame every 125 ms (`ALERT_BLINK_MS / 2`); the alert now takes 148 ms,
so the banner still blinks a little slower than designed. Touch stays responsive: it is polled between strips
throughout.

## 5. Regression (Phase 8)

| Check | Result | Evidence |
|---|---|---|
| All targets build | pass | BUILD `a2c0fc3`: esp32s3 0x1502E0 (30 % free), diag 0x151390; esp32c3 0x1150F0 and esp32c6 0x165960 unchanged; esp32c5 (CSI line removed) 0x197910 (+0x60, the `rr_cov` code); warnings identical to upstream |
| Host tests | pass | AUTO: netlog, gesture, rtc_pcf, memdiag, apcount |
| BLE message format | unchanged | BUILD: no `WD:` line added or removed in `firmware/main` or `PROTOCOL.md` since `a406ec3` |
| Default image on the watch | pass | HW-LOG: `fw=1.7.6`; `clock set from rtc`; battery 100 %, 4,115 mV, on USB, not charging; touch ready; GPS off; phone linked, `notifications on`, Wi-Fi stream; BOOT ×3 → Bluetooth, capture, Wi-Fi started; screen off after 60 s, on at a touch, off again; `wd_lcd` 1,840 left. HW-EYE: "all normal" (time, battery, pages, BOOT, sleep and wake) |
| Upstream screens | unchanged | AUTO: 28 upstream scenes byte-identical through both drawing changes |

## 6. Open risks and next steps

| # | What | Status |
|---|---|---|
| 1 | The four 30-minute soaks (Wi-Fi display on/asleep, Bluetooth, capture) | NOT RUN; procedure in section 3; the 67 minutes of stress above showed no decline |
| 2 | Long-run heap trend beyond ≈1 h | not measured; the per-minute minimum stayed in a 2 KB band for 67 min |
| 3 | Alert frames slower than upstream's 125 ms target | 148 ms after both changes; further gains would come from the remaining `mix`/`fill`/`rrect_line` work |
| 4 | The app's ≈61 s reconnect backoff | app-side, not fixable here; worth reporting upstream |
| 5 | Reading a deleted role task's stack mark | the diagnostics, like upstream's `WD:HEALTH`, read a task handle that its task clears just before deleting itself; a read in that window reads freed memory [INFER: harmless in practice, not observed]; diagnostics images only |
| 6 | PSRAM | off by decision (`memory-audit.md` section 12) |
| 7 | Partition table | unchanged by decision (`memory-audit.md` section 13) |
