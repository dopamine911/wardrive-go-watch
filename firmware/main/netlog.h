// WardriveESP — the watch's recent networks: the last NETLOG_N different Wi-Fi APs heard, one entry per BSSID, newest
// sighting kept. Fixed memory; a full list gives the slot of the AP heard longest ago to the new one. Plain C, no
// ESP-IDF calls and no locking (the caller serializes): tools/netlog_test.c checks it on a PC.
// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#define NETLOG_N 32

typedef struct {
    uint8_t     bssid[6];
    uint8_t     ssid_len;    // 0 = hidden
    char        ssid[33];    // raw bytes as broadcast, NUL-terminated (may hold UTF-8 or anything else)
    int8_t      rssi;        // dBm, from the latest sighting
    uint8_t     ch;
    const char *auth;        // the WD:AP token (a string literal): OPEN, WEP, WPA, WPA2, WPA3, WPA2_WPA3, UNKNOWN
    uint32_t    seen_ms;     // latest sighting (a monotonic clock)
} netlog_rec_t;

typedef struct {
    netlog_rec_t r[NETLOG_N];
    size_t n;
} netlog_t;

/// One sighting. `ssid` holds `ssid_len` bytes (longer is cut to 32).
void netlog_note(netlog_t *l, const uint8_t bssid[6], const uint8_t *ssid, size_t ssid_len, int rssi, uint8_t ch,
                 const char *auth, uint32_t now_ms);
/// Up to `max` entries into `out`, most recently heard first. Returns how many.
size_t netlog_recent(const netlog_t *l, netlog_rec_t *out, size_t max);
