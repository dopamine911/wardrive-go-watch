// WardriveESP — memory and runtime diagnostics (bench builds, CONFIG_WARDRIVEESP_MEMDIAG): one snapshot per interval
// into a fixed ring, each entry sequence-numbered and checksummed so a lost or damaged entry shows. The ring is meant
// to live in RTC memory that survives a software reset, so the minutes before an unexpected reset can be read after it.
// Plain C, no ESP-IDF calls: tools/memdiag_test.c checks it on a PC.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEMDIAG_N       96            // snapshots kept (96 one-minute snapshots: 96 minutes)
#define MEMDIAG_NO_TASK 0xFFFF        // a stack field for a task that doesn't exist

/// Stack high-water marks (bytes never used), in this order.
enum { MD_ST_LCD = 0, MD_ST_ROLE, MD_ST_USB, MD_ST_HEALTH, MD_ST_BTPWR, MD_ST_HOST, MD_ST_WIFI, MD_ST_TIMER, MD_ST_N };

/// Link and screen state bits.
enum { MD_F_BLE = 1, MD_F_NOTIFY = 2, MD_F_USB = 4, MD_F_DARK = 8, MD_F_SCANNING = 16 };

typedef struct {
    uint32_t seq;                 // 1, 2, ... across resets (0 = empty slot)
    uint32_t up_s;                // uptime at the snapshot (falls back to near 0 after a reset)
    uint32_t heap_free;           // free internal heap now
    uint32_t heap_min_iv;         // lowest free internal heap seen in this interval (sampled each second)
    uint32_t heap_min_life;       // lowest since boot (the allocator's own record)
    uint32_t largest_min_iv;      // smallest "largest free internal block" seen in this interval
    uint16_t stack[MD_ST_N];      // bytes never used, MEMDIAG_NO_TASK if the task doesn't exist
    uint16_t allocfail;           // failed allocations since boot
    uint16_t twdt;                // task watchdog timeouts since boot
    uint16_t lcd_frames;          // screen: frames in its last minute window
    uint16_t lcd_avg_ms, lcd_worst_ms;
    uint16_t txdrop_iv;           // BLE output frames dropped in this interval
    uint16_t qdrop_iv;            // Bluetooth or capture records dropped in this interval (queue full)
    uint8_t  msys_min_iv;         // fewest free BLE output buffers seen in this interval
    uint8_t  msys_total;
    uint8_t  q_max_iv;            // deepest the Bluetooth or capture queue got in this interval
    uint8_t  role;                // 0 Wi-Fi, 1 Bluetooth, 2 capture, 3 CSI
    uint8_t  flags;               // MD_F_*
    uint8_t  reserved[3];
    uint32_t aps;                 // unique APs since boot
    uint32_t sweeps;              // Wi-Fi sweeps since boot
    uint32_t sum;                 // checksum over everything above
} memdiag_snap_t;

typedef struct {
    uint32_t magic;
    uint32_t next_seq;
    uint32_t head;                // slot the next snapshot goes into
    memdiag_snap_t s[MEMDIAG_N];
} memdiag_ring_t;

/// Per-interval extremes, folded once a second.
typedef struct {
    uint32_t heap_min, largest_min;
    uint8_t  msys_min, q_max;
    bool     any;
} memdiag_acc_t;

/// True when the ring holds a valid header (it survived a reset). Entries are checked one by one on read.
bool memdiag_ring_valid(const memdiag_ring_t *r);
/// Empty ring; sequence numbers start at 1.
void memdiag_ring_init(memdiag_ring_t *r);
/// Stores a copy of `s` with the next sequence number and its checksum. Returns the sequence number given.
uint32_t memdiag_push(memdiag_ring_t *r, const memdiag_snap_t *s);
/// The i-th snapshot from the oldest (0) to the newest. False for an empty or damaged slot.
bool memdiag_get(const memdiag_ring_t *r, size_t i, memdiag_snap_t *out);
/// The checksum `memdiag_push` stores (exposed for the host test).
uint32_t memdiag_sum(const memdiag_snap_t *s);

void memdiag_acc_reset(memdiag_acc_t *a);
void memdiag_acc_fold(memdiag_acc_t *a, uint32_t heap_free, uint32_t largest, uint8_t msys_free, uint8_t q_depth);

/// One line ("MD seq=... up=..."), no newline. Returns the length written (always < n), 0 if n is too small.
size_t memdiag_format(const memdiag_snap_t *s, char *buf, size_t n);
