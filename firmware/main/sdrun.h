// WardriveESP — offline runs on the microSD card (the Koko V8 / AWOK Dual C5 / Marauder Mini v3 slot on the screen's SPI
// bus, the Ghost Board's on a bus of its own; boards with GPS, so every row has a place). While no host
// holds the board, every Wi-Fi sighting goes to /runs/run_<n>.csv in WiGLE CSV. Rows are queued and written by one
// low-priority task, so the scan and Bluetooth tasks never wait on the card. No card (or any other board): nothing here
// runs and nothing changes.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// One Wi-Fi sighting, as the board reports it to the app (WD:AP) plus where and when.
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];
    int8_t   rssi;
    uint8_t  ch;
    const char *auth;        // the WD:AP token: OPEN, WEP, WPA, WPA2, WPA3, WPA2_WPA3, UNKNOWN (a string literal)
    bool     fix;            // a GPS fix from the last few seconds
    double   lat, lon, alt;
    float    acc_m;
} sdrun_ap_t;

/// At boot, once the screen runs: mount the card in the background if the board has the slot. `board_id` / `board_name`
/// / `fw` go into each file's WiGLE header line.
void sdrun_init(const char *board_id, const char *board_name, const char *fw);
/// A board whose slot has an SPI bus of its own (no screen on it): the Rabbit Labs Ghost Board (ESP32-C6). The bus is
/// set up on SPI2 the first time the card mounts.
void sdrun_init_bus(const char *board_id, const char *board_name, const char *fw, int sck, int miso, int mosi, int cs);
/// Try the mount again (`sd mount`), e.g. after a card went in. False when the board has no slot or one is mounted.
bool sdrun_remount(void);
bool sdrun_mounted(void);
/// Card size and free space in MB (free is refreshed at mount and whenever a run closes).
void sdrun_space(uint32_t *total_mb, uint32_t *free_mb);

/// The last host let go: open a new run (the writer task does the file work). False without a mounted card.
bool sdrun_start(void);
/// A host linked again: the run gets its last rows, then closes.
void sdrun_stop(void);
bool sdrun_active(void);
/// Queue one sighting while a run is open. Never blocks: a full queue drops the row (counted).
void sdrun_ap(const sdrun_ap_t *ap);
/// Rows written to the current (or last) run, rows dropped, and its file name ("" = none yet).
uint32_t sdrun_rows(void);
uint32_t sdrun_dropped(void);
const char *sdrun_file(void);

/// Console helpers (`sd list`, `sd read`, `sd del`). Each runs under the bus lock. Names are file names inside /runs.
typedef void (*sdrun_list_fn)(const char *name, uint32_t size, void *ctx);
int  sdrun_list(sdrun_list_fn fn, void *ctx);                             // count, or -1 when not mounted
int  sdrun_read(const char *name, uint32_t off, uint8_t *buf, size_t len, uint32_t *size);   // bytes read, -1 = error
int  sdrun_delete(const char *name);                                      // 0 ok, -1 missing / error, -2 the open run
/// Closed runs waiting on the card for an app to take them (the open run is not counted).
void sdrun_pending(uint32_t *n, uint32_t *bytes);
/// Times the card stopped answering mid-run and was mounted again.
uint32_t sdrun_remounts(void);
/// `sd test`: write, read back and delete a small file. "ok" or what failed.
const char *sdrun_test(void);
