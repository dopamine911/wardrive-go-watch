// WardriveESP — color status screens over SPI: the Koko Marauder V8's 240x320 ILI9341 and the Marauder Mini v3's
// 128x128 ST7735. Only ever driven once a probe has read the panel's ID; every other board never touches these pins.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// What the dashboard shows, filled by wardrive_bridge.c a few times a second.
typedef struct {
    uint8_t  channel;        // channel being scanned (0 = none)
    char     mode[5];        // "WIFI", "CAP", "BT", "CSI"
    bool     ble, usb;       // who holds the board
    char     holder[17];     // the app's nickname ("" = didn't say)
    uint32_t captures;       // handshakes + PMKIDs this session
    uint32_t bt_devices;     // Bluetooth devices last window (BT mode)
    int      batt_pct;       // -1 = no gauge
    uint32_t aps_live;       // APs heard in the last 20-30 s
    uint32_t aps_session;    // unique APs since boot
    bool     ota;            // a firmware update is coming in: its screen replaces everything else
    bool     ota_installing; // every byte arrived: the board checks the image, installs it and restarts
    uint8_t  ota_pct;        // 0-100 received
    uint32_t ota_kb, ota_total_kb;
    bool     sd_log;         // no host linked and a run is going to the microSD card
    uint32_t sd_rows;        // rows in that run so far
    bool     sd_send;        // the app is taking offline runs off the card: their transfer screen replaces the dashboard
    bool     sd_sent;        // it deleted the last one: SENT for a moment, then back to the dashboard
    uint8_t  sd_run, sd_runs;// run sd_run of sd_runs this time (SENT: sd_runs = runs taken)
    char     sd_file[24];    // the file going now
    uint32_t sd_bytes;       // of that file: the furthest the app has read
    uint32_t sd_size;        // its size on the card
    uint32_t sd_rate;        // bytes a second for this file (0 = not known yet)
    uint32_t sd_sent_ms;     // how long SENT has shown
    uint32_t sd_sent_bytes;  // every run taken this time
} lcd_status_t;

typedef void (*lcd_fill_fn)(lcd_status_t *st);

/// Koko Marauder V8 check: only when a pin of its shared SPI bus already reads pulled up (input-only sense), reads the
/// panel ID over SPI and keeps the bus when it is an ILI9341. Logs what it saw either way.
bool lcd_probe_koko(void);
/// Marauder Mini v3 check: same pulled-up gate, then reads the ST7735's ID (3-wire on the bus's MOSI, then MISO). `stored`:
/// the board was told it is a Mini, which stands in for an ID the panel can't give back (the gate still applies).
bool lcd_probe_mini(bool stored);
/// Waveshare ESP32-S3 Touch AMOLED 2.06 (ESP32-S3 only): resets the CO5300 and opens its QSPI bus. Call only once the
/// board was identified from its board bus; false on every other chip.
bool lcd_open_amoled(void);
/// Splash, then the live dashboard on a low-priority task.
void lcd_start(const char *board, const char *version, lcd_fill_fn fill);
bool lcd_running(void);
/// "240x320" / "128x128" / "410x502" while a screen runs, else "".
const char *lcd_size(void);
/// A Wi-Fi AP sighting (Wi-Fi scan or capture): unique count, "+new" ticker, channel activity.
void lcd_note_ap(const uint8_t *bssid, uint8_t channel);
/// Full-screen flashing banner for `ms` (0 ends it). `soft` banners never replace one already showing. `rgb`: the
/// app's banner color for this alert (the same one the LED ring and the phone's banner use); `police`: red/blue.
void lcd_alert(const char *label, uint32_t ms, bool soft, uint32_t rgb, bool police);
/// Stealth: panel and backlight off.
void lcd_set_stealth(bool on);
/// A touch screen: true while a finger is down, with its panel coordinates. Polled once a frame by the screen task.
typedef bool (*lcd_touch_fn)(int *x, int *y);
/// Hand the screen its touch input (the watch). With one set, the panel goes dark after 30 s without a touch and comes
/// back on the next touch or alert; the scan and Bluetooth carry on either way.
void lcd_set_touch(lcd_touch_fn fn);
/// The bus's microSD slot, once the ILI9341 screen runs and the slot's chip select read pulled up (Koko V8, AWOK Dual
/// C5): its SPI host and chip select. False on every other board.
bool lcd_sd_slot(int *host, int *cs);
/// The panel and the card never share the bus at once: each side holds this around its transfers.
void lcd_bus_lock(void);
void lcd_bus_unlock(void);
