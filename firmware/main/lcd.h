// WardriveESP — color status screens: the Koko Marauder V8's 240x320 ILI9341 and the Marauder Mini v3's 128x128 ST7735
// over SPI, the Waveshare ESP32-S3 Touch AMOLED 2.06's 410x502 CO5300 over QSPI. Only ever driven once the board was
// identified from its hardware; every other board never touches these pins.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "netlog.h"

/// The watch's power: no USB power, charging, or on USB power and not charging (full, or the charger is resting).
enum { LCD_CHARGE_NONE = 0, LCD_CHARGE_ON, LCD_CHARGE_PLUGGED };

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
    uint32_t aps_run;        // unique APs this run: since the board last linked to a host or lost it
    bool     ota;            // a firmware update is coming in: its screen replaces everything else
    bool     ota_installing; // every byte arrived: the board checks the image, installs it and restarts
    uint8_t  ota_pct;        // 0-100 received
    uint32_t ota_kb, ota_total_kb;
    bool     sd_log;         // no host linked and a run is going to the microSD card
    bool     sd_wait_gps;    // no host linked, a card in, no run because the GPS has no fix yet
    uint32_t sd_rows;        // rows in that run so far
    uint32_t sd_waiting;     // closed runs on the card waiting for an app to take them
    bool     sd_send;        // the app is taking offline runs off the card: their transfer screen replaces the dashboard
    bool     sd_sent;        // it deleted the last one: SENT for a moment, then back to the dashboard
    uint8_t  sd_run, sd_runs;// run sd_run of sd_runs this time (SENT: sd_runs = runs taken)
    char     sd_file[24];    // the file going now
    uint32_t sd_bytes;       // of that file: the furthest the app has read
    uint32_t sd_size;        // its size on the card
    uint32_t sd_rate;        // bytes a second for this file (0 = not known yet)
    uint32_t sd_sent_ms;     // how long SENT has shown
    uint32_t sd_sent_bytes;  // every run taken this time
    // The watch (410x502): counts it measures itself, its link and its recent sightings, for the mode it is in
    bool     scanning;       // the scan for `mode` is running
    bool     can_scan;       // a host is listening (the phone subscribed, or a USB session), so a scan can start
    int      batt_mv;        // 0 = unknown
    uint8_t  charge;         // LCD_CHARGE_*: USB power and whether the battery is taking it
    char     ble_name[24];   // the board's advertised name
    uint16_t mtu;            // the phone link's ATT MTU (0 = no phone)
    uint32_t bt_now;         // Bluetooth mode: devices so far in the current 10 s window (bt_devices: the last whole one)
    uint32_t bt_windows;     // Bluetooth mode: windows since the mode started
    uint32_t cap_hs, cap_pmkid, cap_clients;   // capture mode: handshakes, PMKIDs, stations, this capture session
    bool     wall_ok;        // the clock holds real time (the clock chip, or the app's `time`); the fields below are local
    uint8_t  wall_hour, wall_min, wall_wday, wall_mday, wall_mon;   // wday 0 = Sunday, mon 1-12
    uint16_t wall_year;
    uint8_t  nets_n;         // entries in nets, most recently heard first
    netlog_rec_t nets[NETLOG_N];   // Wi-Fi and capture: APs; Bluetooth: devices (ch 0, auth = address type)
} lcd_status_t;

typedef void (*lcd_fill_fn)(lcd_status_t *st);
/// The watch's scan buttons: pause the scan when it runs (start it again when a host listens), or move the board to its
/// next scan mode (Wi-Fi, Bluetooth, capture) with the same commands the app sends.
typedef enum { LCD_SCAN_PAUSE_RESUME, LCD_SCAN_NEXT_MODE } lcd_scan_action_t;
typedef void (*lcd_scan_fn)(lcd_scan_action_t action);

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
/// For the bench diagnostics: the screen task's stack never used (bytes, -1 = no task), whether the panel is dark, the
/// drawing cost of its last one-minute window (touch screens only), and the "lcd:" log line on or off.
long lcd_stack_free(void);
bool lcd_dark(void);
void lcd_diag_last(uint16_t *frames, uint16_t *avg_ms, uint16_t *worst_ms);
void lcd_diag_log(bool on);
/// "240x320" / "128x128" / "410x502" while a screen runs, else "".
const char *lcd_size(void);
/// A Wi-Fi AP sighting (Wi-Fi scan or capture): unique count, "+new" ticker, channel activity.
void lcd_note_ap(const uint8_t *bssid, uint8_t channel);
/// Full-screen flashing banner for `ms` (0 ends it). `soft` banners never replace one already showing. `rgb`: the
/// app's banner color for this alert (the same one the LED ring and the phone's banner use); `police`: red/blue.
void lcd_alert(const char *label, uint32_t ms, bool soft, uint32_t rgb, bool police);
/// Stealth: panel and backlight off.
void lcd_set_stealth(bool on);
/// A touch screen: true while a finger is down, with its panel coordinates. Polled by the screen task.
typedef bool (*lcd_touch_fn)(int *x, int *y);
/// Hand the screen its touch input (the watch): pages, controls, and the panel going dark after its timeout without a
/// touch, back on the next touch or alert. The scan and Bluetooth carry on either way.
void lcd_set_touch(lcd_touch_fn fn);
/// The watch's SCAN and MODE buttons call this (on the screen task).
void lcd_set_scan(lcd_scan_fn fn);
/// The bus's microSD slot, once the ILI9341 screen runs and the slot's chip select read pulled up (Koko V8, AWOK Dual
/// C5): its SPI host and chip select. False on every other board.
bool lcd_sd_slot(int *host, int *cs);
/// The panel and the card never share the bus at once: each side holds this around its transfers.
void lcd_bus_lock(void);
void lcd_bus_unlock(void);
