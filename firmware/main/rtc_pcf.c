// SPDX-License-Identifier: MIT
#include "rtc_pcf.h"

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool unbcd(uint8_t b, unsigned *v) {
    if ((b & 0x0F) > 9 || (b >> 4) > 9) return false;
    *v = (unsigned)((b >> 4) * 10 + (b & 0x0F));
    return true;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
static long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

bool rtc_pcf_encode(uint8_t regs[RTC_PCF_REGS], time_t t) {
    struct tm tm;
    if (!gmtime_r(&t, &tm) || tm.tm_year < 100 || tm.tm_year > 199) return false;
    regs[0] = bcd((unsigned)tm.tm_sec);        // bit 7 (oscillator stopped) clears with this write
    regs[1] = bcd((unsigned)tm.tm_min);
    regs[2] = bcd((unsigned)tm.tm_hour);       // 24-hour mode (CTRL1 bit 1 = 0, the power-on default)
    regs[3] = bcd((unsigned)tm.tm_mday);
    regs[4] = (uint8_t)tm.tm_wday;
    regs[5] = bcd((unsigned)tm.tm_mon + 1);
    regs[6] = bcd((unsigned)tm.tm_year - 100);
    return true;
}

bool rtc_pcf_decode(const uint8_t regs[RTC_PCF_REGS], time_t *t) {
    if (regs[0] & 0x80) return false;
    unsigned s, mi, h, d, mo, y;
    if (!unbcd(regs[0] & 0x7F, &s) || !unbcd(regs[1] & 0x7F, &mi) || !unbcd(regs[2] & 0x3F, &h) ||
        !unbcd(regs[3] & 0x3F, &d) || !unbcd(regs[5] & 0x1F, &mo) || !unbcd(regs[6], &y))
        return false;
    if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return false;
    *t = (time_t)(days_from_civil(2000 + (int)y, mo, d) * 86400L + (long)h * 3600 + (long)mi * 60 + (long)s);
    return true;
}
