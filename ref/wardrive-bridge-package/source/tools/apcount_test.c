// Checks main/apcount.c on a PC: a simulated drive (APs coming into and out of range, each heard many times), then
// parking somewhere empty. Prints true vs estimated counts.
//   zig cc -O2 -I main tools/apcount_test.c main/apcount.c -o apcount_test && ./apcount_test
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apcount.h"

#ifndef NEW_PER_S
#define NEW_PER_S 40   // APs coming into range per second (-DNEW_PER_S=6 for a quiet street)
#endif

static void mac_of(uint32_t id, uint8_t m[6]) {
    m[0] = 0x02; m[1] = (uint8_t)(id >> 24); m[2] = (uint8_t)(id >> 16); m[3] = (uint8_t)(id >> 8); m[4] = (uint8_t)id;
    m[5] = (uint8_t)(id * 37);
}

int main(void) {
    // Drive: every second a new block of APs comes into range and stays heard for 25 s (each one 5 beacons a second).
    const int new_per_s = NEW_PER_S, stay_s = 25, drive_s = 300;
    uint8_t m[6];
    static int last_heard[20000];   // ms, -1 = never
    for (int i = 0; i < 20000; i++) last_heard[i] = -1;
    int worst_session = 0, worst_live = 0;
    for (int t = 0; t < drive_s; t++) {
        for (int ms = 0; ms < 1000; ms += 200) {
            int first = (t - stay_s + 1) * new_per_s; if (first < 0) first = 0;
            for (int id = first; id < (t + 1) * new_per_s; id++) { mac_of((uint32_t)id, m); apc_add(m, (uint32_t)(t * 1000 + ms)); last_heard[id] = t * 1000 + ms; }
        }
        if (t % 30 == 29) {
            int now = t * 1000 + 999, from = (now / 10000 - 2) * 10000, truth_s = (t + 1) * new_per_s, truth_l = 0;
            for (int i = 0; i < 20000; i++) if (last_heard[i] >= from) truth_l++;   // heard in the live window
            int es = (int)apc_session(), el = (int)apc_live((uint32_t)(t * 1000 + 999));
            int ds = abs(es - truth_s) * 1000 / truth_s, dl = abs(el - truth_l) * 1000 / truth_l;
            if (ds > worst_session) worst_session = ds;
            if (dl > worst_live) worst_live = dl;
            printf("t=%3ds  session %6d (true %6d)   live %5d (heard in window %5d)\n", t + 1, es, truth_s, el, truth_l);
        }
    }
    // Park somewhere empty.
    for (int s = 0; s <= 40; s += 10)
        printf("parked +%2ds: live %d, session %u\n", s, (int)apc_live((uint32_t)(drive_s * 1000 + s * 1000)), apc_session());
    printf("worst error: session %d.%d%%, live %d.%d%%\n", worst_session / 10, worst_session % 10, worst_live / 10, worst_live % 10);
    return 0;
}
