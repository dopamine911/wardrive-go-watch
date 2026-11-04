// WardriveESP — what the color screens (240x320, 128x128) draw, one 16-row strip at a time. Plain C with no ESP-IDF
// calls, so the same code renders the panel and the bench preview (tools: lcd_preview.c).
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lcd.h"

#define LCD_W  240                      // the largest panel: strip buffers and hashes are sized for it
#define LCD_H  320
#define LCD_SH 8                        // rows per strip (8: half the DMA strip buffer, 3.8 KB back on a 240-wide panel)
#define LCD_STRIPS (LCD_H / LCD_SH)
#define LCD_ACT_N 42                    // 14 × 2.4 GHz, then 28 × 5 GHz

/// Everything one frame shows, gathered once so every strip of the frame agrees.
typedef struct {
    int w, h;                           // this panel: 240x320 gets the full dashboard, 128x128 the compact one
    lcd_status_t st;
    uint32_t now, t0;                   // ms
    uint32_t live, session, fresh;      // APs in range now, unique this session, new in the last 10 s
    uint32_t run;                       // unique this run (sent to the phone, or saved to the card)
    int cur;                            // activity index of the channel being scanned (-1 = none)
    uint8_t act[LCD_ACT_N];             // channel activity, 0-250
    bool alerting, alert_on;            // banner showing, and its blink phase
    char alert[13];
    uint32_t alert_rgb;                 // the app's banner color (0 = the default red)
    bool alert_police;                  // police: red and blue, swapping each blink
    const char *board, *version;
} lcd_frame_t;

/// Activity index of a channel, or -1.
int lcd_act_index(uint8_t ch);
/// Draw rows y0..y0+LCD_SH-1 of the frame into `strip` (f->w pixels a row, RGB565, byte-swapped for the wire).
void lcd_draw_strip(uint16_t *strip, int y0, const lcd_frame_t *f);
