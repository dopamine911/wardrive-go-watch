// WardriveESP — Wi-Fi AP counts that hold up on a drive: unique APs this session, and APs in range now (heard in the
// last 20-30 s). Fixed memory (HyperLogLog sketches, 2.5 KB total), so a dense city never overflows a table and nothing
// is counted twice. Plain C, no ESP-IDF calls (tools/apcount_test.c checks it on a PC).
// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/// One AP sighting (any frame or scan result naming this BSSID). `now_ms`: a monotonic clock.
void apc_add(const uint8_t bssid[6], uint32_t now_ms);
/// Unique APs since boot (about 3 % error, within a few percent below ~2 500).
uint32_t apc_session(void);
/// APs new to this session since the last apc_run_reset (the screens' per-run count: sent to the phone, or saved to
/// the card). An AP already seen earlier in the session isn't counted again.
uint32_t apc_run(void);
/// A new run starts (the board linked to a host, or lost it).
void apc_run_reset(void);
/// APs heard in the last 20-30 s (about 4 % error): drops back to 0 within 30 s of the last AP.
uint32_t apc_live(uint32_t now_ms);
