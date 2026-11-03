// WardriveESP — 128x32 SSD1306 status screen (the Rabbit Labs Poltergeist's). Only ever driven once board_detect()
// has identified a board that has one; generic boards never touch the pins or allocate anything here.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// What the dashboard shows, filled by wardrive_bridge.c a few times a second.
typedef struct {
    uint8_t  channel;        // channel being scanned (0 = none)
    char     mode[5];        // "WIFI", "CAP", "BT"
    bool     ble, usb;       // who holds the board
    char     holder[17];     // the app's nickname ("" = didn't say)
    uint32_t captures;       // handshakes + PMKIDs this session
    uint32_t bt_devices;     // Bluetooth devices last window (BT mode)
    uint32_t aps_live;       // APs heard in the last 20-30 s
    uint32_t aps_session;    // unique APs since boot
    bool     ota;            // a firmware update is coming in: its screen replaces everything else
    bool     ota_installing; // every byte arrived: the board checks the image, installs it and restarts
    uint8_t  ota_pct;        // 0-100 received
    uint32_t ota_kb, ota_total_kb;
} display_status_t;

typedef void (*display_fill_fn)(display_status_t *st);

/// True when an SSD1306 answers at 0x3C on this pair (both lines must read pulled up). Leaves the bus open on success.
bool display_probe(int sda, int scl);
/// Closes the bus display_probe() opened, when the board turns out not to be one that has this screen.
void display_release(void);
/// Splash, then the live dashboard on a low-priority task.
void display_start(const char *board, const char *version, display_fill_fn fill);
bool display_running(void);
/// A Wi-Fi AP sighting (Wi-Fi scan or capture): unique count, "+new" ticker, channel activity.
void display_note_ap(const uint8_t *bssid, uint8_t channel);
/// Full-width inverted banner that flashes for `ms`, then back to the dashboard.
void display_alert(const char *label, uint32_t ms);
/// Stealth: the screen goes fully dark.
void display_set_stealth(bool on);
/// What the panel shows now, for `screen`: per 8-pixel page, two hex lines of 64 column bytes (bit 0 at the top).
void display_dump(void (*emit)(int page, int x, const char *hex));
