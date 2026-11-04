# Power measurement (issue #2)

How to measure where the watch's battery goes, and what has been measured so far. Evidence labels as in
`waveshare-verification.md`.

## Results so far

### Run A: idle on a desk, unplugged (HW-LOG, diagnostics image `d4ed29a`)

33 minutes. Every minute: screen dark (0 wakes, 0 lit seconds), no phone linked, not scanning (`flags=8`).
Brightness setting 50 %, screen-off 30 s.

| | Just unplugged (660 s) | 2,640 s | Change |
|---|---|---|---|
| Battery voltage | 3,838 mV | 3,669 mV | −169 mV; −130 mV over the last 31 min (≈ −250 mV/h) |
| AXP2101 percent | 46 % | 38 % | held 46 % for 20 min while the voltage fell 120 mV, then ≈ −0.5 %/min |

- The watch drains fast with nothing happening: no screen, no link, no scan.
- The gauge percent lags and then catches up; voltage is the figure to compare. Battery capacity is unknown, so no
  mA figure comes from a drain run.

### Idle suspects (from the code, not yet measured)

| # | Suspect | Shipping behaviour | Bench switch |
|---|---|---|---|
| 1 | Connectable advertising while unlinked | every 30–60 ms (NimBLE's default; `start_advertising` sets no interval). Upstream behaviour on every board, for fast reconnects | `bench adv <ms>` (0 = default) |
| 2 | Panel when dark | display-off (`0x28`) only | `bench panel sleep` adds sleep-in (`0x10`); `bench panel off` back |
| 3 | CPU clock | fixed 160 MHz, no power management | `bench pm dfs` lets it drop to 40 MHz when idle (diagnostics image only); `bench pm off` back |
| 4 | Touch and BOOT polling while dark | every 50 ms | `bench poll <ms>` |

`diag` prints the current state of all four (`adv_ms`, `panel_sleep`, `dark_poll_ms`, `pm`). Side effects seen on the
bench: with `pm dfs` the panel transfer took ≈45 ms per frame instead of 10–25 ms; waking from panel sleep adds 120 ms
(sleep-out time), and the screen comes back normally (HW-EYE).

## Measuring with a USB power meter

With the battery **full**, the power chip stops charging and an inline USB meter reads the watch's own draw directly.
That makes each switch a reading in seconds instead of a 30-minute run.

1. Flash the diagnostics image (`memory-baseline.md`) and charge until `diag` (or the screen's dim bolt) shows *on USB
   power, not charging*: `flags` has 32 (USB power) without 64 (charging). A battery topping up between readings
   adds charge current and spoils them, so check `flags` before each one.
2. Close Wardrive Go, or turn the phone's Bluetooth off, so nothing links (the unlinked state is run A's).
3. Put the meter between the computer and the watch; send commands with `firmware/tools/bench_serial.py` (no
   session: Bluetooth stays up).
4. Read the meter for each row, giving each 20–30 s to settle. Note the average, not the peaks.

| Step | Commands | Read |
|---|---|---|
| Baseline, screen lit | `bench wake` (repeat every 20 s) | mA |
| Baseline, screen dark | wait for screen-off | mA |
| Panel sleep | `bench panel sleep`, `bench wake`, wait for screen-off | mA (dark) |
| Slow polling | `bench poll 200` | mA (dark) |
| Slower adverts | `bench adv 500`, then `bench adv 1000` | mA (dark) |
| CPU power management | `bench pm dfs` | mA (dark) |
| Back to shipping | `bench pm off`, `bench adv 0`, `bench poll 50`, `bench panel off` | mA (should match the dark baseline) |
| Linked, scanning | open Wardrive Go (foreground) | mA, screen dark and lit |

The last row is the wardriving case, for comparison with the idle ones. Then the biggest differences decide which
changes are worth making in the shipping firmware. Each candidate, and its cost:

- Slower adverts mean slower reconnects.
- Power management is shared with every S3 board.
- Panel sleep adds 120 ms to waking.
