// SPDX-License-Identifier: MIT
#include "memdiag.h"

#include <stdio.h>
#include <string.h>

#define MD_MAGIC 0x4D444732u   // "MDG2"; changes whenever memdiag_snap_t's layout changes (MDG1: no battery or wakes)

// FNV-1a over the snapshot up to its checksum field, seeded so an all-zero slot never checks out.
uint32_t memdiag_sum(const memdiag_snap_t *s) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 0x811C9DC5u ^ 0x5A5Au;
    for (size_t i = 0; i < offsetof(memdiag_snap_t, sum); i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

bool memdiag_ring_valid(const memdiag_ring_t *r) {
    return r->magic == MD_MAGIC && r->head < MEMDIAG_N && r->next_seq != 0;
}

void memdiag_ring_init(memdiag_ring_t *r) {
    memset(r, 0, sizeof(*r));
    r->magic = MD_MAGIC;
    r->next_seq = 1;
}

uint32_t memdiag_push(memdiag_ring_t *r, const memdiag_snap_t *s) {
    memdiag_snap_t *d = &r->s[r->head];
    *d = *s;
    d->seq = r->next_seq++;
    if (r->next_seq == 0) r->next_seq = 1;   // 0 marks an empty slot
    d->sum = memdiag_sum(d);
    r->head = (r->head + 1) % MEMDIAG_N;
    return d->seq;
}

bool memdiag_get(const memdiag_ring_t *r, size_t i, memdiag_snap_t *out) {
    if (i >= MEMDIAG_N) return false;
    const memdiag_snap_t *s = &r->s[(r->head + i) % MEMDIAG_N];   // head is the oldest once the ring has wrapped
    if (!s->seq || s->sum != memdiag_sum(s)) return false;
    *out = *s;
    return true;
}

void memdiag_acc_reset(memdiag_acc_t *a) { memset(a, 0, sizeof(*a)); }

void memdiag_acc_fold(memdiag_acc_t *a, uint32_t heap_free, uint32_t largest, uint8_t msys_free, uint8_t q_depth) {
    if (!a->any || heap_free < a->heap_min) a->heap_min = heap_free;
    if (!a->any || largest < a->largest_min) a->largest_min = largest;
    if (!a->any || msys_free < a->msys_min) a->msys_min = msys_free;
    if (q_depth > a->q_max) a->q_max = q_depth;
    a->any = true;
}

static int st(uint16_t v) { return v == MEMDIAG_NO_TASK ? -1 : (int)v; }

size_t memdiag_format(const memdiag_snap_t *s, char *buf, size_t n) {
    int k = snprintf(buf, n,
                     "MD seq=%lu up=%lu heap=%lu heap_min_iv=%lu heap_min=%lu largest_min_iv=%lu "
                     "st_lcd=%d st_role=%d st_usb=%d st_health=%d st_btpwr=%d st_host=%d st_wifi=%d st_timer=%d "
                     "allocfail=%u twdt=%u lcd_frames=%u lcd_avg=%u lcd_worst=%u txdrop=%u qdrop=%u "
                     "msys_min=%u/%u q_max=%u role=%u flags=%u aps=%lu sweeps=%lu "
                     "batt_mv=%u batt_pct=%d lit_s=%u wakes=%u wakes_touch=%u wakes_boot=%u",
                     (unsigned long)s->seq, (unsigned long)s->up_s, (unsigned long)s->heap_free,
                     (unsigned long)s->heap_min_iv, (unsigned long)s->heap_min_life, (unsigned long)s->largest_min_iv,
                     st(s->stack[MD_ST_LCD]), st(s->stack[MD_ST_ROLE]), st(s->stack[MD_ST_USB]),
                     st(s->stack[MD_ST_HEALTH]), st(s->stack[MD_ST_BTPWR]), st(s->stack[MD_ST_HOST]),
                     st(s->stack[MD_ST_WIFI]), st(s->stack[MD_ST_TIMER]), (unsigned)s->allocfail, (unsigned)s->twdt,
                     (unsigned)s->lcd_frames, (unsigned)s->lcd_avg_ms, (unsigned)s->lcd_worst_ms,
                     (unsigned)s->txdrop_iv, (unsigned)s->qdrop_iv, (unsigned)s->msys_min_iv, (unsigned)s->msys_total,
                     (unsigned)s->q_max_iv, (unsigned)s->role, (unsigned)s->flags, (unsigned long)s->aps,
                     (unsigned long)s->sweeps, (unsigned)s->batt_mv,
                     s->batt_pct == MEMDIAG_NO_BATT ? -1 : (int)s->batt_pct, (unsigned)s->lit_s_iv,
                     (unsigned)s->wakes_iv, (unsigned)s->wakes_touch_iv, (unsigned)s->wakes_boot_iv);
    if (k < 0 || (size_t)k >= n) { if (n) buf[0] = '\0'; return 0; }
    return (size_t)k;
}
