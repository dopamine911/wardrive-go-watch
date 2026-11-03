// Checks main/netlog.c on a PC: one entry per BSSID, eviction of the AP heard longest ago, newest-first order, SSID
// limits, and the millisecond clock wrapping. Exits non-zero on the first failure.
//   cc -O2 -Wall -Wextra -I main tools/netlog_test.c main/netlog.c -o netlog_test && ./netlog_test
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netlog.h"

static int s_fail;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)

static void mac(uint32_t id, uint8_t m[6]) { m[0] = 0x02; m[1] = 0; m[2] = (uint8_t)(id >> 24); m[3] = (uint8_t)(id >> 16); m[4] = (uint8_t)(id >> 8); m[5] = (uint8_t)id; }
static void note(netlog_t *l, uint32_t id, const char *ssid, int rssi, uint32_t t) {
    uint8_t m[6];
    mac(id, m);
    netlog_note(l, m, (const uint8_t *)ssid, strlen(ssid), rssi, 6, "WPA2", t);
}
static uint32_t id_of(const netlog_rec_t *r) { return (uint32_t)r->bssid[2] << 24 | (uint32_t)r->bssid[3] << 16 | (uint32_t)r->bssid[4] << 8 | r->bssid[5]; }

static void same_bssid_is_one_entry(void) {
    static netlog_t l;
    memset(&l, 0, sizeof(l));
    note(&l, 1, "home", -70, 100);
    note(&l, 1, "home-renamed", -40, 200);
    netlog_rec_t out[NETLOG_N];
    CHECK(netlog_recent(&l, out, NETLOG_N) == 1);
    CHECK(out[0].rssi == -40 && out[0].seen_ms == 200 && !strcmp(out[0].ssid, "home-renamed"));
}

static void full_list_evicts_longest_unheard(void) {
    static netlog_t l;
    memset(&l, 0, sizeof(l));
    for (uint32_t i = 0; i < NETLOG_N; i++) note(&l, i, "x", -60, 1000 + i);
    note(&l, 0, "x", -60, 5000);           // AP 0 heard again: AP 1 is now the oldest
    note(&l, 999, "new", -50, 6000);       // list full: AP 1 makes room
    netlog_rec_t out[NETLOG_N];
    size_t n = netlog_recent(&l, out, NETLOG_N);
    CHECK(n == NETLOG_N);
    bool has0 = false, has1 = false, has999 = false;
    for (size_t i = 0; i < n; i++) { has0 |= id_of(&out[i]) == 0; has1 |= id_of(&out[i]) == 1; has999 |= id_of(&out[i]) == 999; }
    CHECK(has0 && !has1 && has999);
}

static void recent_is_newest_first_and_capped(void) {
    static netlog_t l;
    memset(&l, 0, sizeof(l));
    const uint32_t order[] = { 5, 1, 9, 3, 7 };          // ids heard at times 10, 20, 30, 40, 50
    for (int i = 0; i < 5; i++) note(&l, order[i], "x", -60, 10u * (i + 1));
    netlog_rec_t out[3];
    CHECK(netlog_recent(&l, out, 3) == 3);
    CHECK(id_of(&out[0]) == 7 && id_of(&out[1]) == 3 && id_of(&out[2]) == 9);
}

static void ssid_limits(void) {
    static netlog_t l;
    memset(&l, 0, sizeof(l));
    note(&l, 1, "0123456789012345678901234567890123456789", -60, 1);   // 40 bytes: cut to 32
    note(&l, 2, "", -60, 2);                                           // hidden
    netlog_rec_t out[2];
    CHECK(netlog_recent(&l, out, 2) == 2);
    CHECK(out[1].ssid_len == 32 && strlen(out[1].ssid) == 32 && !strncmp(out[1].ssid, "01234567890123456789012345678901", 32));
    CHECK(out[0].ssid_len == 0 && out[0].ssid[0] == '\0');
}

static void clock_wrap(void) {
    static netlog_t l;
    memset(&l, 0, sizeof(l));
    for (uint32_t i = 0; i < NETLOG_N; i++) note(&l, i, "x", -60, 0xFFFFFF00u + i);   // just before the wrap
    note(&l, 100, "after", -60, 0x10);                                                 // after it: evicts AP 0
    netlog_rec_t out[NETLOG_N];
    size_t n = netlog_recent(&l, out, NETLOG_N);
    CHECK(n == NETLOG_N);
    CHECK(id_of(&out[0]) == 100);                    // newest despite the smaller raw time
    CHECK(id_of(&out[n - 1]) == 1);                  // AP 0 was evicted; AP 1 is now the oldest
}

int main(void) {
    same_bssid_is_one_entry();
    full_list_evicts_longest_unheard();
    recent_is_newest_first_and_capped();
    ssid_limits();
    clock_wrap();
    printf(s_fail ? "netlog: %d failure(s)\n" : "netlog: all checks passed\n", s_fail);
    return s_fail ? 1 : 0;
}
