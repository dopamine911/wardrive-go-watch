// WardriveESP — AP counts from HyperLogLog sketches: a 1024-register one for the session, and three 512-register
// ones that each cover 10 s for "in range now" (the live count merges the current and the two previous windows).
// Below 2.5 × registers the estimate switches to linear counting, which is close to exact at the sizes a drive sees.
// Registers are bytes written from the radio tasks and read by the beacon / screen tasks: a torn read only ever
// reads an older value of one register, so no lock.
// SPDX-License-Identifier: MIT

#include <math.h>
#include <string.h>
#include "apcount.h"

#define S_P 10                 // session: 1024 registers
#define L_P 9                  // live: 512 registers per window
#define S_M (1u << S_P)
#define L_M (1u << L_P)
#define WIN_MS 10000u
#define WINS 3

static uint8_t s_reg[S_M];
static uint8_t l_reg[WINS][L_M];
static uint32_t l_epoch[WINS];   // which 10 s window each slot holds (+1, so 0 = never used)

static uint64_t mix(const uint8_t b[6]) {   // splitmix64 finalizer over the 48-bit address
    uint64_t z = 0;
    for (int i = 0; i < 6; i++) z = (z << 8) | b[i];
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void put(uint8_t *reg, int p, uint64_t h) {
    uint32_t idx = (uint32_t)(h >> (64 - p));
    uint64_t w = (h << p) | (1ull << (p - 1));   // the guard bit caps the rank
    uint8_t rank = (uint8_t)(__builtin_clzll(w) + 1);
    if (rank > reg[idx]) reg[idx] = rank;
}

static uint32_t slot_for(uint32_t now_ms) {
    uint32_t epoch = now_ms / WIN_MS + 1, s = epoch % WINS;
    if (l_epoch[s] != epoch) { memset(l_reg[s], 0, L_M); l_epoch[s] = epoch; }   // a new window starts empty
    return s;
}

void apc_add(const uint8_t bssid[6], uint32_t now_ms) {
    uint64_t h = mix(bssid);
    put(s_reg, S_P, h);
    put(l_reg[slot_for(now_ms)], L_P, h);
}

// From the register sum Σ 2^-M and the count of empty registers.
static uint32_t estimate_from(float sum, uint32_t zeros, uint32_t m) {
    if (zeros == m) return 0;
    float alpha = 0.7213f / (1.0f + 1.079f / (float)m);
    float e = alpha * (float)m * (float)m / sum;
    if (e <= 2.5f * (float)m && zeros) e = (float)m * logf((float)m / (float)zeros);
    return (uint32_t)(e + 0.5f);
}

uint32_t apc_session(void) {
    float sum = 0;
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < S_M; i++) {
        sum += ldexpf(1.0f, -(int)s_reg[i]);
        if (!s_reg[i]) zeros++;
    }
    return estimate_from(sum, zeros, S_M);
}

// The current window and the two before it, merged register by register on the fly (no buffer, so the beacon and
// the screen can both ask at once).
uint32_t apc_live(uint32_t now_ms) {
    uint32_t epoch = now_ms / WIN_MS + 1;
    const uint8_t *win[WINS];
    int n = 0;
    for (int k = 0; k < WINS; k++) {
        uint32_t e = epoch > (uint32_t)k ? epoch - (uint32_t)k : 0;
        if (e && l_epoch[e % WINS] == e) win[n++] = l_reg[e % WINS];   // windows with nothing heard are skipped
    }
    float sum = 0;
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < L_M; i++) {
        uint8_t r = 0;
        for (int k = 0; k < n; k++) if (win[k][i] > r) r = win[k][i];
        sum += ldexpf(1.0f, -(int)r);
        if (!r) zeros++;
    }
    return estimate_from(sum, zeros, L_M);
}
