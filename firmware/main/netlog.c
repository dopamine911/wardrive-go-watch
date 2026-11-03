// WardriveESP — the watch's recent networks (see netlog.h).
// SPDX-License-Identifier: MIT

#include <string.h>
#include "netlog.h"

#define SSID_MAX 32

void netlog_note(netlog_t *l, const uint8_t bssid[6], const uint8_t *ssid, size_t ssid_len, int rssi, uint8_t ch,
                 const char *auth, uint32_t now_ms) {
    netlog_rec_t *slot = NULL;
    for (size_t i = 0; i < l->n && !slot; i++)
        if (!memcmp(l->r[i].bssid, bssid, 6)) slot = &l->r[i];
    if (!slot && l->n < NETLOG_N) slot = &l->r[l->n++];
    if (!slot) {   // full: the AP heard longest ago makes room (wrap-safe age, the clock is a free-running ms count)
        slot = &l->r[0];
        for (size_t i = 1; i < l->n; i++)
            if (now_ms - l->r[i].seen_ms > now_ms - slot->seen_ms) slot = &l->r[i];
    }
    if (ssid_len > SSID_MAX) ssid_len = SSID_MAX;
    memcpy(slot->bssid, bssid, 6);
    memcpy(slot->ssid, ssid, ssid_len);
    slot->ssid[ssid_len] = '\0';
    slot->ssid_len = (uint8_t)ssid_len;
    slot->rssi = (int8_t)(rssi < -128 ? -128 : rssi > 127 ? 127 : rssi);
    slot->ch = ch;
    slot->auth = auth;
    slot->seen_ms = now_ms;
}

size_t netlog_recent(const netlog_t *l, netlog_rec_t *out, size_t max) {
    // Insertion sort by age (newest first); at most NETLOG_N entries. Ages are taken from the newest sighting so the
    // order holds across the clock wrapping.
    uint32_t newest = 0;
    for (size_t i = 0; i < l->n; i++)
        if (!i || l->r[i].seen_ms - newest < UINT32_MAX / 2) newest = l->r[i].seen_ms;
    size_t n = 0;
    for (size_t i = 0; i < l->n; i++) {
        uint32_t age = newest - l->r[i].seen_ms;
        size_t at = n;
        while (at > 0 && newest - out[at - 1].seen_ms > age) at--;
        if (at >= max) continue;
        size_t keep = n < max ? n : max - 1;
        memmove(&out[at + 1], &out[at], (keep - at) * sizeof(*out));
        out[at] = l->r[i];
        if (n < max) n++;
    }
    return n;
}
