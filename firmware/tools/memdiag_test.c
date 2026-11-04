// Host test for main/memdiag.c.  cc -O2 -Wall -Wextra -I main tools/memdiag_test.c main/memdiag.c && ./a.out
#include <stdio.h>
#include <string.h>

#include "memdiag.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static memdiag_snap_t snap(uint32_t up) {
    memdiag_snap_t s;
    memset(&s, 0, sizeof(s));
    s.up_s = up;
    s.heap_free = 100000 + up;
    for (int i = 0; i < MD_ST_N; i++) s.stack[i] = (uint16_t)(1000 + i);
    return s;
}

int main(void) {
    static memdiag_ring_t r;
    memdiag_snap_t out;

    // An uninitialized (zeroed or random) ring is not valid; an initialized one is, and it is empty
    memset(&r, 0, sizeof(r));
    CHECK(!memdiag_ring_valid(&r));
    memset(&r, 0xA5, sizeof(r));
    CHECK(!memdiag_ring_valid(&r));
    memdiag_ring_init(&r);
    CHECK(memdiag_ring_valid(&r));
    for (size_t i = 0; i < MEMDIAG_N; i++) CHECK(!memdiag_get(&r, i, &out));

    // Fewer than N: read back in order, oldest first, with sequence numbers 1..k
    for (uint32_t k = 0; k < 10; k++) {
        memdiag_snap_t s = snap(k * 60);
        CHECK(memdiag_push(&r, &s) == k + 1);
    }
    size_t got = 0;
    for (size_t i = 0; i < MEMDIAG_N; i++)
        if (memdiag_get(&r, i, &out)) { CHECK(out.seq == got + 1 && out.up_s == got * 60); got++; }
    CHECK(got == 10);

    // Past N: only the newest N remain, still oldest first, sequence numbers contiguous
    for (uint32_t k = 10; k < MEMDIAG_N + 37; k++) { memdiag_snap_t s = snap(k * 60); memdiag_push(&r, &s); }
    uint32_t prev = 0;
    got = 0;
    for (size_t i = 0; i < MEMDIAG_N; i++) {
        CHECK(memdiag_get(&r, i, &out));
        if (prev) CHECK(out.seq == prev + 1);
        prev = out.seq;
        got++;
    }
    CHECK(got == MEMDIAG_N);
    CHECK(prev == MEMDIAG_N + 37);
    CHECK(memdiag_get(&r, 0, &out) && out.seq == 38);

    // A reset keeps the ring: the header stays valid and numbering continues where it was
    CHECK(memdiag_ring_valid(&r));
    { memdiag_snap_t s = snap(5); CHECK(memdiag_push(&r, &s) == MEMDIAG_N + 38); }

    // A damaged entry (one flipped bit) is refused; its neighbours still read
    size_t slot = (r.head + 3) % MEMDIAG_N;
    r.s[slot].heap_free ^= 0x10;
    CHECK(!memdiag_get(&r, 3, &out));
    CHECK(memdiag_get(&r, 2, &out) && memdiag_get(&r, 4, &out));
    // So is a slot whose checksum field alone was hit
    r.s[slot].heap_free ^= 0x10;
    CHECK(memdiag_get(&r, 3, &out));
    r.s[slot].sum ^= 1;
    CHECK(!memdiag_get(&r, 3, &out));
    CHECK(!memdiag_get(&r, MEMDIAG_N, &out));   // out of range

    // A header pointing outside the ring is not trusted
    r.head = MEMDIAG_N;
    CHECK(!memdiag_ring_valid(&r));

    // Interval extremes: minima and maximum across folds; a reset starts over (no stale minimum of 0)
    memdiag_acc_t a;
    memdiag_acc_reset(&a);
    memdiag_acc_fold(&a, 90000, 40000, 30, 2);
    memdiag_acc_fold(&a, 85000, 45000, 28, 9);
    memdiag_acc_fold(&a, 95000, 38000, 33, 1);
    CHECK(a.heap_min == 85000 && a.largest_min == 38000 && a.msys_min == 28 && a.q_max == 9);
    memdiag_acc_reset(&a);
    memdiag_acc_fold(&a, 120000, 60000, 36, 0);
    CHECK(a.heap_min == 120000 && a.largest_min == 60000 && a.msys_min == 36 && a.q_max == 0);

    // The line: every field present, missing tasks as -1, and never written past the buffer
    memdiag_snap_t s = snap(3600);
    s.seq = 7;
    s.stack[MD_ST_ROLE] = MEMDIAG_NO_TASK;
    char line[512];
    size_t n = memdiag_format(&s, line, sizeof(line));
    CHECK(n > 0 && n == strlen(line));
    CHECK(strstr(line, "MD seq=7 up=3600 ") == line);
    CHECK(strstr(line, " st_role=-1 ") && strstr(line, " st_lcd=1000 ") && strstr(line, " sweeps=0 "));
    // Power fields: an unknown battery level prints -1, the rest as given
    s.batt_pct = MEMDIAG_NO_BATT;
    s.batt_mv = 3806; s.lit_s_iv = 42; s.wakes_iv = 5; s.wakes_touch_iv = 3; s.wakes_boot_iv = 1;
    n = memdiag_format(&s, line, sizeof(line));
    CHECK(n > 0 && strstr(line, " batt_mv=3806 batt_pct=-1 lit_s=42 wakes=5 wakes_touch=3 wakes_boot=1"));
    s.batt_pct = 14;
    CHECK(memdiag_format(&s, line, sizeof(line)) > 0 && strstr(line, " batt_pct=14 "));
    char small[40];
    memset(small, 'x', sizeof(small));
    CHECK(memdiag_format(&s, small, sizeof(small)) == 0 && small[0] == '\0');
    // The longest possible line still fits the firmware's 512-byte line buffer (a line that doesn't fit is dropped)
    memdiag_snap_t big;
    memset(&big, 0xFF, sizeof(big));
    for (int i = 0; i < MD_ST_N; i++) big.stack[i] = MEMDIAG_NO_TASK - 1;
    big.batt_pct = 100;
    CHECK(memdiag_format(&big, line, 512) > 0);

    // A ring left in RTC memory by an older layout is not trusted (its entries would be misread)
    memdiag_ring_init(&r);
    r.magic = 0x4D444731u;   // "MDG1", the layout before the battery and wake fields
    CHECK(!memdiag_ring_valid(&r));

    if (!fails) printf("memdiag: all checks passed (snapshot %zu bytes, ring %zu bytes)\n", sizeof(memdiag_snap_t),
                       sizeof(memdiag_ring_t));
    return fails ? 1 : 0;
}
