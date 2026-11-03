// Checks main/gesture.c on a PC: taps do nothing, a still finger holds once, swipes need their travel and direction,
// and a finger that moved never holds. Samples come every 20 ms, as the watch polls while a finger is down.
// Exits non-zero on the first failure.
//   cc -O2 -Wall -Wextra -I main tools/gesture_test.c main/gesture.c -o gesture_test && ./gesture_test
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include "gesture.h"

static int s_fail;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)
#define POLL_MS 20

/// A finger going from (x0,y0) to (x1,y1) over `ms`, then lifting. Returns the gestures seen, in order (up to 4).
static int stroke(int x0, int y0, int x1, int y1, uint32_t ms, gesture_t out[4]) {
    gest_t g = {0};
    int n = 0;
    uint32_t t = 1000;
    for (uint32_t e = 0; e <= ms; e += POLL_MS) {
        int x = x0 + (int)((long)(x1 - x0) * (long)e / (long)(ms ? ms : 1));
        int y = y0 + (int)((long)(y1 - y0) * (long)e / (long)(ms ? ms : 1));
        gesture_t r = gest_feed(&g, true, x, y, t + e);
        if (r && n < 4) out[n++] = r;
    }
    gesture_t r = gest_feed(&g, false, 0, 0, t + ms + POLL_MS);
    if (r && n < 4) out[n++] = r;
    return n;
}

int main(void) {
    gesture_t g[4];

    // A tap is nothing, however it lands.
    CHECK(stroke(200, 250, 200, 250, 100, g) == 0);
    CHECK(stroke(200, 250, 210, 255, 300, g) == 0);   // jitter inside the slop

    // A still finger holds once, at GEST_HOLD_MS, and lifting afterwards adds nothing.
    CHECK(stroke(200, 250, 200, 250, 1500, g) == 1 && g[0] == GEST_HOLD);
    CHECK(stroke(200, 250, 220, 270, 800, g) == 1 && g[0] == GEST_HOLD);   // small drift (20 px) still holds
    CHECK(stroke(200, 250, 200, 250, GEST_HOLD_MS - POLL_MS, g) == 0);     // lifted just short of a hold

    // Swipes: enough travel, the main axis clearly dominant, quick enough.
    CHECK(stroke(300, 250, 300 - GEST_SWIPE_PX, 250, 200, g) == 1 && g[0] == GEST_LEFT);
    CHECK(stroke(100, 250, 100 + GEST_SWIPE_PX, 250, 200, g) == 1 && g[0] == GEST_RIGHT);
    CHECK(stroke(200, 400, 200, 400 - GEST_SWIPE_PX, 200, g) == 1 && g[0] == GEST_UP);
    CHECK(stroke(200, 100, 200, 100 + GEST_SWIPE_PX, 200, g) == 1 && g[0] == GEST_DOWN);
    CHECK(stroke(300, 250, 300 - (GEST_SWIPE_PX - 1), 250, 200, g) == 0);   // one pixel short
    CHECK(stroke(300, 300, 200, 200, 200, g) == 0);                         // diagonal: neither axis wins
    CHECK(stroke(300, 250, 100, 250, GEST_SWIPE_MS + 200, g) == 0);         // a slow drag

    // A finger that moved never holds, even if it comes back and stays still.
    {
        gest_t s = {0};
        uint32_t t = 0;
        gest_feed(&s, true, 200, 250, t);
        gest_feed(&s, true, 260, 250, t += POLL_MS);   // past the slop
        gesture_t any = GEST_NONE;
        for (int i = 0; i < 60; i++) { gesture_t r = gest_feed(&s, true, 200, 250, t += POLL_MS); if (r) any = r; }
        CHECK(any == GEST_NONE);
        CHECK(gest_hold_pct(&s, t) == 0);
    }

    // Hold progress climbs to 100 for the button's fill, and is 0 once the hold fired or with no finger.
    {
        gest_t s = {0};
        gest_feed(&s, true, 50, 50, 0);
        CHECK(gest_hold_pct(&s, 0) == 0);
        CHECK(gest_hold_pct(&s, GEST_HOLD_MS / 2) == 50);
        CHECK(gest_feed(&s, true, 50, 50, GEST_HOLD_MS) == GEST_HOLD);
        CHECK(gest_hold_pct(&s, GEST_HOLD_MS) == 0);
        gest_feed(&s, false, 0, 0, GEST_HOLD_MS + POLL_MS);
        CHECK(gest_hold_pct(&s, GEST_HOLD_MS + POLL_MS) == 0);
    }

    printf(s_fail ? "gesture: %d failure(s)\n" : "gesture: all checks passed\n", s_fail);
    return s_fail ? 1 : 0;
}
