// WardriveESP — the watch's touch gestures (see gesture.h).
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include "gesture.h"

gesture_t gest_feed(gest_t *g, bool down, int x, int y, uint32_t now_ms) {
    if (down && !g->down) {   // finger lands
        *g = (gest_t){ .down = true, .x0 = x, .y0 = y, .x = x, .y = y, .t0 = now_ms };
        return GEST_NONE;
    }
    if (down) {
        g->x = x;
        g->y = y;
        if (abs(x - g->x0) > GEST_SLOP_PX || abs(y - g->y0) > GEST_SLOP_PX) g->moved = true;
        if (!g->moved && !g->held && now_ms - g->t0 >= GEST_HOLD_MS) { g->held = true; return GEST_HOLD; }
        return GEST_NONE;
    }
    if (!g->down) return GEST_NONE;
    g->down = false;   // finger lifts: judged from the last position it was seen at
    if (g->held || !g->moved || now_ms - g->t0 > GEST_SWIPE_MS) return GEST_NONE;
    int dx = g->x - g->x0, dy = g->y - g->y0, ax = abs(dx), ay = abs(dy);
    if (ax >= GEST_SWIPE_PX && ax > ay * 3 / 2) return dx < 0 ? GEST_LEFT : GEST_RIGHT;
    if (ay >= GEST_SWIPE_PX && ay > ax * 3 / 2) return dy < 0 ? GEST_UP : GEST_DOWN;
    return GEST_NONE;
}

int gest_hold_pct(const gest_t *g, uint32_t now_ms) {
    if (!g->down || g->moved || g->held) return 0;
    uint32_t t = now_ms - g->t0;
    return t >= GEST_HOLD_MS ? 100 : (int)(t * 100 / GEST_HOLD_MS);
}
