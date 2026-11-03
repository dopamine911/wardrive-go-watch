// WardriveESP — the watch's touch gestures, from polled finger samples. Swipes turn pages; a button needs a hold, so a
// sleeve or a bump brushing the screen never presses anything. A plain tap is deliberately nothing. Plain C, no
// ESP-IDF calls: tools/gesture_test.c checks it on a PC.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GEST_SLOP_PX   24     // movement that turns a press into a swipe attempt (no hold after it)
#define GEST_SWIPE_PX  80     // travel along the main axis for a swipe
#define GEST_SWIPE_MS  1000   // slower than this is a drag, not a swipe
#define GEST_HOLD_MS   600    // still this long: a hold (a button press)

typedef enum { GEST_NONE = 0, GEST_LEFT, GEST_RIGHT, GEST_UP, GEST_DOWN, GEST_HOLD } gesture_t;

typedef struct {
    bool down, moved, held;
    int x0, y0, x, y;         // where the finger went down, where it is (or left)
    uint32_t t0;              // when it went down
} gest_t;

/// One poll: finger down (and where) or up. Returns a gesture when one completes: a swipe on release, a hold the moment
/// the finger has stayed still for GEST_HOLD_MS (once; releasing afterwards returns nothing).
gesture_t gest_feed(gest_t *g, bool down, int x, int y, uint32_t now_ms);
/// While a still finger is down: how far towards a hold, 0-100 (for the button's fill). 0 otherwise.
int gest_hold_pct(const gest_t *g, uint32_t now_ms);
