// Host test for main/rtc_pcf.c.  cc -O2 -Wall -Wextra -I main tools/rtc_pcf_test.c main/rtc_pcf.c && ./a.out
#include <stdio.h>
#include <string.h>

#include "rtc_pcf.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    uint8_t r[RTC_PCF_REGS];
    time_t t;

    // 2026-11-03 19:34:22 UTC, a Tuesday (weekday 2): 1793734462
    CHECK(rtc_pcf_encode(r, 1793734462));
    CHECK(r[0] == 0x22 && r[1] == 0x34 && r[2] == 0x19 && r[3] == 0x03 && r[4] == 2 && r[5] == 0x11 && r[6] == 0x26);
    CHECK(rtc_pcf_decode(r, &t) && t == 1793734462);

    // Every day boundary that trips date maths: leap day, century year 2000 (leap), 2100 (not leap; out of range), year end
    static const time_t ts[] = { 946684800, 951782400, 1709164800, 1767225599, 1767225600, 4102444799 };
    for (size_t i = 0; i < sizeof(ts) / sizeof(*ts); i++) {
        CHECK(rtc_pcf_encode(r, ts[i]));
        CHECK(rtc_pcf_decode(r, &t) && t == ts[i]);
    }
    // Round trip every 7 hours 13 minutes 11 seconds for 25 years: no skipped or doubled day anywhere
    for (time_t x = 946684800; x < 946684800 + 25L * 366 * 86400; x += 7 * 3600 + 13 * 60 + 11) {
        if (!rtc_pcf_encode(r, x) || !rtc_pcf_decode(r, &t) || t != x) { printf("FAIL round trip at %ld\n", (long)x); fails++; break; }
    }

    // Out of the chip's range: refused, registers untouched
    memset(r, 0xAA, sizeof(r));
    CHECK(!rtc_pcf_encode(r, 946684799));    // 1999-12-31
    CHECK(!rtc_pcf_encode(r, 4102444800));   // 2100-01-01
    CHECK(r[0] == 0xAA && r[6] == 0xAA);

    // Oscillator stopped: the time is not trustworthy
    rtc_pcf_encode(r, 1793734462);
    r[0] |= 0x80;
    CHECK(!rtc_pcf_decode(r, &t));
    // Garbage that a blank or unpowered chip can hold
    rtc_pcf_encode(r, 1793734462);
    r[1] = 0x6A; CHECK(!rtc_pcf_decode(r, &t));   // 6A: a digit over 9
    rtc_pcf_encode(r, 1793734462);
    r[5] = 0x13; CHECK(!rtc_pcf_decode(r, &t));   // month 13
    rtc_pcf_encode(r, 1793734462);
    r[3] = 0x00; CHECK(!rtc_pcf_decode(r, &t));   // day 0
    memset(r, 0x00, sizeof(r)); CHECK(!rtc_pcf_decode(r, &t));   // all zero: month 0
    memset(r, 0xFF, sizeof(r)); CHECK(!rtc_pcf_decode(r, &t));

    // Bits the chip doesn't use in a field are ignored (24-hour hours has bit 6/7 clear, but a stray one mustn't shift it)
    rtc_pcf_encode(r, 1793734462);
    r[2] |= 0x40;
    CHECK(rtc_pcf_decode(r, &t) && t == 1793734462);

    if (!fails) printf("rtc_pcf: all checks passed\n");
    return fails ? 1 : 0;
}
