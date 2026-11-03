// WardriveESP — what the color screens (240x320, 128x128, the watch's 410x502) draw, one 16-row strip at a time. Plain C
// with no ESP-IDF calls, so the same code renders the panel and the bench preview (tools: lcd_preview.c).
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lcd.h"

#define LCD_W  410                      // the largest panel (the watch): strip buffers and hashes are sized for it
#define LCD_H  502
#define LCD_SH 16                       // rows per strip
#define LCD_STRIPS ((LCD_H + LCD_SH - 1) / LCD_SH)   // the last strip of a panel may be partial (502 = 31 x 16 + 6)

// The watch: four pages, swiped left and right (the first is a plain clock). Buttons on the controls page act on a hold
// (gesture.h).
enum { LCD_PAGE_CLOCK = 0, LCD_PAGE_DASH, LCD_PAGE_NETS, LCD_PAGE_CTRL, LCD_PAGES };
enum { LCD_BTN_NONE = -1, LCD_BTN_SCAN = 0, LCD_BTN_MODE, LCD_BTN_BRIGHT, LCD_BTN_SLEEP, LCD_BTNS };
#define LCD_NETS_ROWS 5                 // networks page: rows on screen; swipe up and down for more
#define LCD_ACT_N 42                    // 14 × 2.4 GHz, then 28 × 5 GHz

/// Everything one frame shows, gathered once so every strip of the frame agrees.
typedef struct {
    int w, h;                           // this panel: 410x502 the watch's pages, 240x320 the full dashboard, 128x128 the compact one
    lcd_status_t st;
    uint32_t now, t0;                   // ms
    uint32_t live, session, fresh;      // APs in range now, unique this session, new in the last 10 s
    int cur;                            // activity index of the channel being scanned (-1 = none)
    uint8_t act[LCD_ACT_N];             // channel activity, 0-250
    bool alerting, alert_on;            // banner showing, and its blink phase
    char alert[13];
    uint32_t alert_rgb;                 // the app's banner color (0 = the default red)
    bool alert_police;                  // police: red and blue, swapping each blink
    const char *board, *version;
    // The watch
    uint8_t page;                       // LCD_PAGE_*
    uint8_t net_first;                  // networks page: the entry in the top row
    int8_t hold_btn;                    // controls page: the button under a still finger (LCD_BTN_NONE: none)
    uint8_t hold_pct;                   // how far that finger is towards pressing it, 0-100
    uint8_t bright_pct;                 // screen brightness setting
    uint16_t sleep_s;                   // screen-off timeout setting
} lcd_frame_t;

/// Activity index of a channel, or -1.
int lcd_act_index(uint8_t ch);
/// Draw rows y0..y0+LCD_SH-1 of the frame into `strip` (f->w pixels a row, RGB565, byte-swapped for the wire).
void lcd_draw_strip(uint16_t *strip, int y0, const lcd_frame_t *f);
/// The watch's controls page: the button at (x, y), or LCD_BTN_NONE. The layout lives with the drawing code.
int lcd_watch_button_at(int x, int y);
