// WardriveESP — the watch's clock chip (NXP PCF85063, I2C 0x51): its seven time registers (0x04..0x0A) to and from Unix
// time (UTC). Plain C, no ESP-IDF calls: tools/rtc_pcf_test.c checks it on a PC.
// Register layout: Waveshare's SensorLib PCF85063Constants.h (SEC 0x04 .. YEAR 0x0A), BCD, seconds bit 7 = oscillator stopped.
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define RTC_PCF_REG_SEC 0x04   // first of the seven time registers
#define RTC_PCF_REGS    7      // seconds, minutes, hours, day, weekday (0 = Sunday), month (1-12), year (00-99 = 2000-2099)

/// UTC seconds since 1970 into the seven registers. Years outside 2000-2099 are refused (false, regs untouched).
bool rtc_pcf_encode(uint8_t regs[RTC_PCF_REGS], time_t t);
/// The seven registers into UTC seconds. False when the oscillator-stopped flag is set (the time can't be trusted) or any
/// field is out of range.
bool rtc_pcf_decode(const uint8_t regs[RTC_PCF_REGS], time_t *t);
