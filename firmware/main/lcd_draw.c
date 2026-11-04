// WardriveESP — the color screens' scenes: boot splash (radar sweep), live dashboard, alert banner, firmware update,
// offline runs going to the phone, each in a 240x320 and a compact 128x128 layout.
// Anti-aliased text from lcd_fonts.h; everything is drawn per 16-row strip, clipped, straight into the wire buffer.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>
#include "lcd_draw.h"
#include "lcd_fonts.h"
#include "lcd_sprites.h"

#define W  s_w
#define H  s_h
#define SH LCD_SH

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK  RGB(0, 0, 0)
#define C_NAVY   RGB(8, 18, 34)
#define C_HEAD0  RGB(12, 28, 50)
#define C_HEAD1  RGB(4, 10, 20)
#define C_PANEL  RGB(14, 24, 38)
#define C_EDGE   RGB(34, 52, 76)
#define C_FAINT  RGB(44, 58, 78)
#define C_DIM    RGB(122, 142, 164)
#define C_WHITE  RGB(255, 255, 255)
#define C_CYAN   RGB(0, 229, 255)
#define C_TEAL   RGB(0, 96, 120)
#define C_MAG    RGB(255, 46, 214)
#define C_GREEN  RGB(60, 255, 140)
#define C_AMBER  RGB(255, 178, 36)
#define C_YEL    RGB(255, 232, 0)
#define C_BLUE   RGB(86, 146, 255)
#define C_RED    RGB(255, 36, 60)
#define C_BLOOD  RGB(96, 0, 14)
#define C_PINK   RGB(255, 79, 163)
#define C_PLUM   RGB(64, 0, 38)

static uint16_t *s_px;
static int s_y0, s_w = LCD_W, s_h = LCD_H;

// Angle of (dx, dy) in 1/256 turns, counterclockwise from +x with y down (within about a quarter of a step).
static int iatan2(int dy, int dx) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (!ax && !ay) return 0;
    int mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    int z = mn * 256 / mx;
    int a = (32 * z + 11 * z * (256 - z) / 256) / 256;   // atan(z), 0-32
    if (ay > ax) a = 64 - a;
    if (dx < 0) a = 128 - a;
    if (dy < 0) a = 256 - a;
    return a & 255;
}

// ── pixels ──
static inline uint16_t sw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static inline bool in_strip(int x, int y) { return (unsigned)x < (unsigned)W && y >= s_y0 && y < s_y0 + SH; }

static uint16_t mix(uint16_t a, uint16_t b, int t, int n) {   // a → b, step t of n
    if (n <= 0 || t >= n) return b;
    if (t <= 0) return a;
    int r = ((a >> 11) * (n - t) + (b >> 11) * t) / n, g = (((a >> 5) & 63) * (n - t) + ((b >> 5) & 63) * t) / n,
        bl = ((a & 31) * (n - t) + (b & 31) * t) / n;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// `a` is coverage 0-16.
static inline void blend(int x, int y, uint16_t c, int a) {
    if (a <= 0 || !in_strip(x, y)) return;
    uint16_t *p = &s_px[(y - s_y0) * W + x];
    *p = a >= 16 ? sw(c) : sw(mix(sw(*p), c, a, 16));
}

static void fill(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (x + w > W) w = W - x;
    int ya = y < s_y0 ? s_y0 : y, yb = y + h > s_y0 + SH ? s_y0 + SH : y + h;
    if (w <= 0 || ya >= yb) return;
    uint16_t v = sw(c);
    for (int j = ya; j < yb; j++) {
        uint16_t *row = s_px + (j - s_y0) * W + x;
        for (int i = 0; i < w; i++) row[i] = v;
    }
}
static void fill_a(int x, int y, int w, int h, uint16_t c, int a) {
    int ya = y < s_y0 ? s_y0 : y, yb = y + h > s_y0 + SH ? s_y0 + SH : y + h;
    for (int j = ya; j < yb; j++) for (int i = x; i < x + w; i++) blend(i, j, c, a);
}
// Vertical gradient c1 (row y) → c2 (row y + h - 1).
static void vgrad(int x, int y, int w, int h, uint16_t c1, uint16_t c2) {
    int ya = y < s_y0 ? s_y0 : y, yb = y + h > s_y0 + SH ? s_y0 + SH : y + h;
    for (int j = ya; j < yb; j++) fill(x, j, w, 1, mix(c1, c2, j - y, h - 1));
}
static void hgrad(int x, int y, int w, int h, uint16_t c1, uint16_t c2) {
    if (y + h <= s_y0 || y >= s_y0 + SH) return;
    for (int i = 0; i < w; i++) fill(x + i, y, 1, h, mix(c1, c2, i, w - 1));
}

// Coverage (0-16, 4×4 samples) of pixel (i, j) by a rounded rectangle.
static int rr_cov(int i, int j, int x, int y, int w, int h, int r) {
    if (i < x || i >= x + w || j < y || j >= y + h) return 0;
    int cx = i < x + r ? x + r : i >= x + w - r ? x + w - r : -1;
    int cy = j < y + r ? y + r : j >= y + h - r ? y + h - r : -1;
    if (cx < 0 || cy < 0) return 16;
    int n = 0, rr = r * 8 * r * 8;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int dx = i * 8 + sx * 2 + 1 - cx * 8, dy = j * 8 + sy * 2 + 1 - cy * 8;
            if (dx * dx + dy * dy <= rr) n++;
        }
    return n;
}
static void rrect(int x, int y, int w, int h, int r, uint16_t c, int a) {
    int ya = y < s_y0 ? s_y0 : y, yb = y + h > s_y0 + SH ? s_y0 + SH : y + h;
    for (int j = ya; j < yb; j++)
        for (int i = x; i < x + w; i++) {
            int cov = rr_cov(i, j, x, y, w, h, r);
            if (cov) blend(i, j, c, cov * a / 16);
        }
}
static void rrect_line(int x, int y, int w, int h, int r, int t, uint16_t c, int a) {
    int ya = y < s_y0 ? s_y0 : y, yb = y + h > s_y0 + SH ? s_y0 + SH : y + h;
    for (int j = ya; j < yb; j++)
        for (int i = x; i < x + w; i++) {
            if (i >= x + t + r && i < x + w - t - r && j >= y + t && j < y + h - t) { i = x + w - t - r - 1; continue; }
            int cov = rr_cov(i, j, x, y, w, h, r) - rr_cov(i, j, x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0);
            if (cov > 0) blend(i, j, c, cov * a / 16);
        }
}
// Disc (th = 0) or ring of thickness th, anti-aliased.
static void circle(int cx, int cy, int r, int th, uint16_t c, int a) {
    if (cy + r < s_y0 || cy - r >= s_y0 + SH) return;
    int ya = cy - r - 1 < s_y0 ? s_y0 : cy - r - 1, yb = cy + r + 1 >= s_y0 + SH ? s_y0 + SH - 1 : cy + r + 1;
    int ro = r * 8 * r * 8, ri = th ? (r - th) * 8 * (r - th) * 8 : -1;
    for (int j = ya; j <= yb; j++)
        for (int i = cx - r - 1; i <= cx + r + 1; i++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = i * 8 + sx * 2 + 1 - cx * 8 - 4, dy = j * 8 + sy * 2 + 1 - cy * 8 - 4, d = dx * dx + dy * dy;
                    if (d <= ro && d > ri) n++;
                }
            if (n) blend(i, j, c, n * a / 16);
        }
}

// ── text ──
static const lcd_glyph_t *glyph_of(const lcd_font_t *f, char c) {
    if ((uint8_t)c < f->first || (uint8_t)c > f->last) c = '?';
    if ((uint8_t)c < f->first || (uint8_t)c > f->last) return NULL;
    return &f->g[(uint8_t)c - f->first];
}
static int text_w(const lcd_font_t *f, const char *s, int sp) {
    int w = 0;
    for (; *s; s++) { const lcd_glyph_t *g = glyph_of(f, *s); if (g) w += g->adv + sp; }
    return w > 0 ? w - sp : 0;
}
enum { SOLID, VERT, HORIZ };
// Text with its baseline at `base`. VERT shades c1 (cap top) → c2 (baseline), HORIZ c1 (left) → c2 (right end).
static void text_ex(const lcd_font_t *f, int x, int base, const char *s, int sp, uint16_t c1, uint16_t c2, int mode, int a) {
    int top = base - f->ascent, span = text_w(f, s, sp), x0 = x, cap = f->ascent * 3 / 4;
    int bold = f == &font_small;   // its 1 px strokes were hard to read on the panels: each pixel also lands 1 px right
    if (top + f->height <= s_y0 || top >= s_y0 + SH) return;
    for (; *s; s++) {
        const lcd_glyph_t *g = glyph_of(f, *s);
        if (!g) continue;
        int gy = top + g->yo, gx = x + g->xo;
        if (g->w && gy < s_y0 + SH && gy + g->h > s_y0) {
            int ra = s_y0 - gy > 0 ? s_y0 - gy : 0, rb = s_y0 + SH - gy < g->h ? s_y0 + SH - gy : g->h;
            for (int row = ra; row < rb; row++) {
                uint16_t c = c1;
                if (mode == VERT) c = mix(c1, c2, gy + row - (base - cap), cap);
                for (int col = 0; col < g->w; col++) {
                    int k = row * g->w + col;
                    int v = (f->bits[g->off + k / 2] >> ((k & 1) ? 0 : 4)) & 15;
                    if (!v) continue;
                    if (mode == HORIZ) c = mix(c1, c2, gx + col - x0, span);
                    blend(gx + col, gy + row, c, (v * a + 7) / 15);
                    if (bold) blend(gx + col + 1, gy + row, c, (v * a + 7) / 15);
                }
            }
        }
        x += g->adv + sp;
    }
}
static void text(const lcd_font_t *f, int x, int base, const char *s, uint16_t c) { text_ex(f, x, base, s, 0, c, c, SOLID, 16); }
static void text_r(const lcd_font_t *f, int xr, int base, const char *s, uint16_t c) { text(f, xr - text_w(f, s, 0), base, s, c); }
// A soft halo: the text smeared around itself, faint.
static void glow(const lcd_font_t *f, int x, int base, const char *s, uint16_t c, int a) {
    static const int8_t D[][2] = { {-3,0},{3,0},{0,-3},{0,3},{-2,-2},{2,-2},{-2,2},{2,2},{-1,0},{1,0},{0,-1},{0,1} };
    for (size_t i = 0; i < sizeof(D) / sizeof(D[0]); i++) text_ex(f, x + D[i][0], base + D[i][1], s, 0, c, c, SOLID, a);
}

// ── pieces ──
static void background(void) {
    vgrad(0, s_y0, W, SH, mix(C_NAVY, C_BLACK, s_y0, H), mix(C_NAVY, C_BLACK, s_y0 + SH - 1, H));
    for (int y = 8; y < H; y += 16) {   // HUD dot grid
        if (y < s_y0 || y >= s_y0 + SH) continue;
        for (int x = 8; x < W; x += 16) blend(x, y, C_FAINT, 9);
    }
}

static void esp_pill(int x, int y) {
    int pw = text_w(&font_title, "ESP", 0) + 10;
    rrect(x, y, pw, 20, 5, C_CYAN, 16);
    text(&font_title, x + 5, y + 16, "ESP", C_BLACK);
}

static void battery(int pct, int x, int y) {
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    uint16_t c = pct > 40 ? C_GREEN : pct > 15 ? C_AMBER : C_RED;
    rrect_line(x, y, 36, 18, 4, 1, C_DIM, 16);
    rrect(x + 36, y + 5, 3, 8, 1, C_DIM, 16);
    int lw = 32 * pct / 100;
    if (lw > 0) rrect(x + 2, y + 2, lw, 14, 3, mix(c, C_BLACK, 7, 16), 16);
    if (lw > 0) rrect(x + 2, y + 2, lw, 3, 2, c, 10);
    char b[8];
    snprintf(b, sizeof(b), "%d", pct);
    text(&font_small, x + 18 - text_w(&font_small, b, 0) / 2, y + 14, b, C_WHITE);
}

static void header(const lcd_frame_t *f) {
    vgrad(0, 0, W, 42, C_HEAD0, C_HEAD1);
    text_ex(&font_head, 9, 29, "WARDRIVE", 0, C_CYAN, C_MAG, HORIZ, 16);
    esp_pill(9 + text_w(&font_head, "WARDRIVE", 0) + 6, 11);
    battery(f->st.batt_pct, 196, 12);
    hgrad(0, 41, W, 2, C_CYAN, C_MAG);
    int gx = (int)((f->now / 6) % (W + 80)) - 40;   // a glint runs along the edge
    for (int i = -40; i <= 40; i++) {
        int a = 12 - (i < 0 ? -i : i) * 12 / 40;
        blend(gx + i, 41, C_WHITE, a);
        blend(gx + i, 42, C_WHITE, a / 2);
    }
}

static void status_pill(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    bool sd = !st->usb && !st->ble && st->sd_log;   // offline run going to the microSD card
    uint16_t c = st->usb ? C_GREEN : st->ble ? C_CYAN : sd ? C_MAG : C_AMBER;
    rrect(8, 50, 224, 28, 14, c, 3);
    rrect_line(8, 50, 224, 28, 14, 1, c, 11);
    bool linked = st->usb || st->ble;
    int pulse = linked ? 16 : (int)((f->now / 90) % 16);
    if (!linked && pulse > 8) pulse = 16 - pulse;
    circle(25, 64, 9, 0, c, linked ? 4 : pulse / 2);
    circle(25, 64, 5, 0, c, linked ? 16 : 8 + pulse);
    uint32_t up = (f->now - f->t0) / 1000;
    char b[12];
    if (up < 3600) snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(up / 60), (unsigned long)(up % 60));
    else snprintf(b, sizeof(b), "%luh%02lu", (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
    int clock_x = 222 - text_w(&font_small, b, 0);
    text(&font_small, clock_x, 69, b, C_DIM);
    const char *what = st->usb ? "USB" : st->ble ? "BLUETOOTH" : sd ? "SAVING TO SD" :
                       st->sd_wait_gps ? "NO GPS FIX" : "NOT SAVING";   // no phone and no card: nothing is kept
    text_ex(&font_mid, 38, 71, what, 1, c, c, SOLID, 16);
    if (linked && st->holder[0]) {
        char h[sizeof(st->holder)];
        strncpy(h, st->holder, sizeof(h) - 1);
        h[sizeof(h) - 1] = 0;
        int hx = 38 + text_w(&font_mid, what, 1) + 7, room = clock_x - 8 - hx;
        if (text_w(&font_mid, h, 0) > room) {   // shortened to fit, with an ellipsis
            char t[sizeof(h) + 3];
            for (size_t n = strlen(h); n > 0; n--) {
                h[n - 1] = 0;
                snprintf(t, sizeof(t), "%s...", h);
                if (text_w(&font_mid, t, 0) <= room) break;
            }
            strncpy(h, t, sizeof(h) - 1);
        }
        text(&font_mid, hx, 71, h, C_WHITE);
    }
}

/// Saving an offline run to the card: the board itself is collecting (linked, the phone is).
static bool collecting(const lcd_status_t *st) { return !st->usb && !st->ble && st->sd_log; }

// Two counts side by side at one size: APs in range now, and this run's unique APs (SENT to the phone, SAVED to the
// card, or only SEEN). Both step down a font together when either gets long.
/// A count that stays big: exact to 99999, then 100K-9999K, then 10M up. The count fonts hold digits only, so the
/// K / M is drawn after in the title font.
static const char *count_short(char *b, size_t n, unsigned long v) {
    if (v < 100000) { snprintf(b, n, "%lu", v); return ""; }
    if (v < 10000000) { snprintf(b, n, "%lu", v / 1000); return "K"; }
    snprintf(b, n, "%lu", v / 1000000);
    return "M";
}

static int count_w(const lcd_font_t *nf, const char *b, const char *unit) {
    return text_w(nf, b, 1) + (*unit ? 2 + text_w(&font_title, unit, 0) : 0);
}

static void ap_count(const lcd_font_t *nf, int x, int base, const char *b, const char *unit) {
    glow(nf, x, base, b, C_TEAL, 3);
    text_ex(nf, x, base, b, 1, C_WHITE, C_CYAN, VERT, 16);
    if (*unit) text(&font_title, x + text_w(nf, b, 1) + 2, base, unit, C_CYAN);
}

static void ap_block(const lcd_frame_t *f) {
    bool col = collecting(&f->st);
    const int cw = 106, rx = 126;   // column width, right column x
    fill(10, 87, 3, 11, C_CYAN);
    text_ex(&font_small, 18, 97, "IN RANGE", 1, C_DIM, C_DIM, SOLID, 16);
    fill(rx - 8, 87, 3, 11, col ? C_MAG : C_TEAL);
    bool linked = f->st.usb || f->st.ble;
    const char *rl = linked ? "SENT" : col ? "SAVED" : "SEEN";
    text_ex(&font_small, rx, 97, rl, 1, C_DIM, C_DIM, SOLID, 16);
    if (f->fresh) {   // new in the last 10 s, after the right label
        char p[12];
        snprintf(p, sizeof(p), "+%lu", (unsigned long)f->fresh);
        int px = rx + text_w(&font_small, rl, 1) + 6, pw = text_w(&font_small, p, 0) + 10;
        if (px + pw <= W - 4) {
            rrect(px, 85, pw, 15, 7, C_GREEN, 4);
            rrect_line(px, 85, pw, 15, 7, 1, C_GREEN, 14);
            text(&font_small, px + 5, 97, p, C_GREEN);
        }
    }
    char a[16], b[16];
    const char *ua = count_short(a, sizeof(a), f->live), *ub = count_short(b, sizeof(b), f->run);
    static const lcd_font_t *const FS[] = { &font_huge, &font_big, &font_num, &font_head };
    static const int BASE[] = { 158, 150, 144, 140 };
    int k = 0;
    while (k < 3 && (count_w(FS[k], a, ua) > cw || count_w(FS[k], b, ub) > cw)) k++;
    fill_a(rx - 10, 106, 1, 52, C_EDGE, 10);
    ap_count(FS[k], 10, BASE[k], a, ua);
    ap_count(FS[k], rx - 2, BASE[k], b, ub);
}

static void tile(int x, int y, const char *label, const char *value, const char *unit, uint16_t accent) {
    rrect(x, y, 112, 36, 7, C_PANEL, 16);
    rrect_line(x, y, 112, 36, 7, 1, C_EDGE, 16);
    rrect(x + 4, y + 8, 3, 20, 1, accent, 16);
    text_ex(&font_small, x + 14, y + 14, label, 1, C_DIM, C_DIM, SOLID, 16);
    text(&font_med, x + 14, y + 32, value, C_WHITE);
    if (unit && *unit) text(&font_small, x + 18 + text_w(&font_med, value, 0), y + 31, unit, accent);
}

static void tiles(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    char b[16];
    uint16_t mc = !strcmp(st->mode, "BT") ? C_BLUE : !strcmp(st->mode, "CAP") ? C_MAG : !strcmp(st->mode, "CSI") ? C_AMBER : C_CYAN;
    tile(6, 168, "MODE", st->mode[0] ? st->mode : "--", NULL, mc);
    if (st->channel) snprintf(b, sizeof(b), "%u", st->channel);
    else snprintf(b, sizeof(b), "--");
    tile(122, 168, "CHANNEL", b, st->channel ? (st->channel <= 14 ? "2.4 GHz" : "5 GHz") : NULL, C_YEL);
    snprintf(b, sizeof(b), "%lu", (unsigned long)st->captures);
    tile(6, 208, "CAPTURES", b, NULL, st->captures ? C_MAG : C_EDGE);
    snprintf(b, sizeof(b), "%lu", (unsigned long)st->bt_devices);
    tile(122, 208, "BT DEVICES", b, NULL, C_BLUE);
}

static void spectrum(const lcd_frame_t *f) {
    const int base = 296, top = 40;
    hgrad(6, base + 1, 228, 1, C_EDGE, C_EDGE);
    for (int i = 0; i < LCD_ACT_N; i++) {
        int x = i < 14 ? 8 + i * 6 : 96 + (i - 14) * 5, bw = i < 14 ? 5 : 4;
        int h = f->act[i] * top / 250;
        if (i == f->cur) {
            if (h < 4) h = 4;
            vgrad(x, base - h, bw, h, C_YEL, C_AMBER);
            fill_a(x - 1, base - h - 2, bw + 2, h + 2, C_YEL, 3);
            for (int k = 0; k < 3; k++) fill(x + k * bw / 6, base + 3 + k, bw - k * bw / 3, 1, C_YEL);
        } else if (f->act[i] < 6) {
            fill(x, base - 2, bw, 2, C_FAINT);
        } else {
            for (int y = 0; y < h; y++) fill(x, base - 1 - y, bw, 1, mix(C_TEAL, C_MAG, y, top));
            fill_a(x, base - h - 1, bw, 1, C_WHITE, 8);
        }
    }
    text(&font_small, 8, 314, "2.4 GHz", C_DIM);
    text(&font_small, 96, 314, "5 GHz", C_DIM);
    if (f->st.sd_waiting) {   // runs on the card for the app to take, in the version's place
        char b[24];
        snprintf(b, sizeof(b), "%lu RUN%s ON SD", (unsigned long)f->st.sd_waiting, f->st.sd_waiting == 1 ? "" : "S");
        text_r(&font_small, 234, 314, b, C_MAG);
    } else {
        text_r(&font_small, 234, 314, f->version, C_FAINT);
    }
}

static void dashboard(const lcd_frame_t *f) {
    background();
    header(f);
    status_pill(f);
    ap_block(f);
    tiles(f);
    spectrum(f);
}

// The radar beam (one turn in 1.6 s) leading a fading wedge.
static void sweep(int cx, int cy, int R, uint32_t t) {
    int ang = (int)(t * 256 / 1600) & 255;
    const int TRAIL = 48;
    int ya = cy - R > s_y0 ? cy - R : s_y0, yb = cy + R < s_y0 + SH ? cy + R : s_y0 + SH;
    for (int y = ya; y < yb; y++)
        for (int x = cx - R; x < cx + R; x++) {
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 > R * R || d2 < 9) continue;
            int behind = (ang - iatan2(dy, dx)) & 255;
            if (behind < 2) blend(x, y, C_WHITE, 14);
            else if (behind < TRAIL) blend(x, y, C_CYAN, (TRAIL - behind) * 9 / TRAIL);
        }
}

static void splash(const lcd_frame_t *f) {
    background();
    uint32_t t = f->now - f->t0;
    const int cx = W / 2, cy = 124;
    for (int r = 34; r <= 102; r += 34) circle(cx, cy, r, 1, C_TEAL, 12);
    fill_a(cx - 104, cy, 208, 1, C_TEAL, 8);
    fill_a(cx, cy - 104, 1, 208, C_TEAL, 8);
    sweep(cx, cy, 102, t);
    circle(cx, cy, 4, 0, C_WHITE, 16);
    int hw = text_w(&font_logo, "WARDRIVE", 2);
    glow(&font_logo, (W - hw) / 2, 266, "WARDRIVE", C_TEAL, 2);
    text_ex(&font_logo, (W - hw) / 2, 266, "WARDRIVE", 2, C_CYAN, C_MAG, HORIZ, 16);
    esp_pill((W - text_w(&font_title, "ESP", 0) - 10) / 2, 274);
    char b[48];
    snprintf(b, sizeof(b), "%s  %s", f->board, f->version);
    text(&font_small, (W - text_w(&font_small, b, 0)) / 2, 314, b, C_DIM);
}

// Filled warning triangle (apex at tx, ty), edges in quarter pixels for smooth sides, with its exclamation mark.
static void warn_triangle(int tx, int ty, int th, int half, uint16_t fg, uint16_t mark) {
    for (int j = 0; j < th; j++) {
        int hw4 = (j * half * 4) / th;
        int full = hw4 / 4, part = hw4 % 4;
        fill(tx - full, ty + j, 2 * full + 1, 1, fg);
        blend(tx - full - 1, ty + j, fg, part * 4);
        blend(tx + full + 1, ty + j, fg, part * 4);
    }
    int bw = th / 9 | 1;
    rrect(tx - bw / 2, ty + th * 31 / 100, bw, th * 40 / 100, bw / 2, mark, 16);
    circle(tx, ty + th * 83 / 100, bw * 6 / 10 + 1, 0, mark, 16);
}

// The alert's color: the app's banner color, so the screen, the LED ring and the phone's banner match. Police swaps
// red and blue every blink, like the banner's light bar. [dark] = the same hue, dimmed, for the gradient's far end.
static void alert_colors(const lcd_frame_t *f, uint16_t *c, uint16_t *dark) {
    uint32_t rgb = f->alert_rgb;
    if (f->alert_police) *c = f->alert_on ? C_RED : C_BLUE;
    else *c = rgb ? RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF) : C_RED;
    *dark = mix(*c, C_BLACK, 62, 100);
}

static void alert(const lcd_frame_t *f) {
    uint16_t c, dark;
    alert_colors(f, &c, &dark);
    bool lit = f->alert_on || f->alert_police;   // police lights stay lit, they swap color instead of blinking
    uint16_t fg = lit ? C_WHITE : c;
    if (lit) vgrad(0, s_y0, W, SH, mix(c, dark, s_y0, H), mix(c, dark, s_y0 + SH - 1, H));
    else { background(); vgrad(0, s_y0, W, SH, mix(dark, C_BLACK, s_y0, H), mix(dark, C_BLACK, s_y0 + SH - 1, H)); }
    rrect_line(6, 6, W - 12, H - 12, 16, 3, fg, 16);
    // warning triangle
    warn_triangle(W / 2, 62, 84, 48, fg, lit ? c : C_BLACK);
    const lcd_font_t *lf = text_w(&font_head, f->alert, 1) <= W - 28 ? &font_head : &font_title;
    text_ex(lf, (W - text_w(lf, f->alert, 1)) / 2, 202, f->alert, 1, fg, fg, SOLID, 16);
    text_ex(&font_small, (W - text_w(&font_small, "ALERT FROM THE APP", 1)) / 2, 236, "ALERT FROM THE APP", 1, fg, fg,
            SOLID, 10);
}

// ── adult-toy alert: the app's toy-shower sprite raining down a hot pink screen ──
static void sprite(const lcd_sprite_t *sp, int x, int y) {
    if (y + sp->h <= s_y0 || y >= s_y0 + SH) return;
    int ra = s_y0 - y > 0 ? s_y0 - y : 0, rb = s_y0 + SH - y < sp->h ? s_y0 + SH - y : sp->h;
    for (int row = ra; row < rb; row++)
        for (int col = 0; col < sp->w; col++) {
            int k = row * sp->w + col;
            int a = (sp->alpha[k / 2] >> ((k & 1) ? 0 : 4)) & 15;
            if (a) blend(x + col, y + row, sp->px[k], (a * 16 + 7) / 15);
        }
}

static bool is_toy(const char *label) { return strstr(label, "TOY") != NULL; }

static void toy_alert(const lcd_frame_t *f) {
    bool mini = W < 200;
    const lcd_sprite_t *sp = mini ? &spr_toy_s : &spr_toy;
    vgrad(0, s_y0, W, SH, mix(C_PINK, C_PLUM, s_y0, H), mix(C_PINK, C_PLUM, s_y0 + SH - 1, H));
    int n = mini ? 6 : 9;
    for (int k = 0; k < n; k++) {   // each one its own lane, speed and start, falling and wrapping around
        uint32_t seed = (uint32_t)(k + 1) * 2654435761u;
        int lane = (W - sp->w) * k / (n - 1);
        int x = lane + (int)((seed >> 9) % 9) - 4;
        int speed = (mini ? 45 : 90) + (int)((seed >> 15) % 70);   // px per second
        int span = H + sp->h;
        int y = (int)(((f->now + (seed >> 3) % 5000) * (uint32_t)speed / 1000) % (uint32_t)span) - sp->h;
        sprite(sp, x, y);
    }
    if (mini) {
        rrect(4, 44, W - 8, 40, 10, C_BLACK, 10);
        rrect_line(4, 44, W - 8, 40, 10, 1, C_PINK, 16);
        const lcd_font_t *lf = text_w(&font_title, f->alert, 0) <= W - 14 ? &font_title : &font_small;
        text(lf, (W - text_w(lf, f->alert, 0)) / 2, 64, f->alert, C_WHITE);
        text_ex(&font_small, (W - text_w(&font_small, "NEARBY", 1)) / 2, 79, "NEARBY", 1, C_PINK, C_PINK, SOLID, 16);
    } else {
        rrect(14, 128, W - 28, 66, 16, C_BLACK, 10);
        rrect_line(14, 128, W - 28, 66, 16, 2, C_PINK, 16);
        const lcd_font_t *lf = text_w(&font_head, f->alert, 1) <= W - 40 ? &font_head : &font_title;
        int lx = (W - text_w(lf, f->alert, 1)) / 2;
        glow(lf, lx, 164, f->alert, C_PINK, 4);
        text_ex(lf, lx, 164, f->alert, 1, C_WHITE, C_WHITE, SOLID, 16);
        text_ex(&font_small, (W - text_w(&font_small, "DETECTED NEARBY", 1)) / 2, 184, "DETECTED NEARBY", 1, C_PINK, C_PINK,
                SOLID, 16);
    }
}

// ── 128x128 ──
static uint16_t link_color(const lcd_status_t *st) {
    return st->usb ? C_GREEN : st->ble ? C_CYAN : st->sd_log ? C_MAG : C_AMBER;
}

static void mini_dashboard(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    char b[24];
    background();
    vgrad(0, 0, W, 19, C_HEAD0, C_HEAD1);
    uint16_t lc = link_color(st);   // who holds the board: green USB, cyan Bluetooth, magenta saving to SD, amber nobody
    bool linked = st->usb || st->ble, col = collecting(st);
    text_ex(&font_title, 4, 15, "WARDRIVE", 0, C_CYAN, C_MAG, HORIZ, 16);
    hgrad(0, 19, W, 1, C_CYAN, C_MAG);
    int pulse = linked ? 16 : (int)((f->now / 90) % 16);
    if (!linked && pulse > 8) pulse = 16 - pulse;
    circle(118, 9, 6, 0, lc, linked ? 4 : pulse / 2);
    circle(118, 9, 3, 0, lc, linked ? 16 : 8 + pulse);

    // What the board is doing, in a pill like the big screens' (in the link's color).
    const char *state = st->usb ? "USB" : st->ble ? "BLUETOOTH" : col ? "SAVING TO SD" :
                        st->sd_wait_gps ? "NO GPS FIX" : "NOT SAVING";
    rrect(3, 23, W - 6, 15, 7, lc, 3);
    rrect_line(3, 23, W - 6, 15, 7, 1, lc, 11);
    text_ex(&font_small, (W - text_w(&font_small, state, 1)) / 2, 35, state, 1, lc, lc, SOLID, 16);

    // The line above names the big number (what this run saved while saving to the card, APs in range otherwise) and
    // puts the other count beside it.
    char o[12];
    const char *ou = count_short(o, sizeof(o), col ? f->live : f->run);
    const char *big_label = col ? "SAVED" : "IN RANGE";
    text(&font_small, 4, 52, big_label, col ? C_MAG : C_DIM);
    if (linked || col) {
        snprintf(b, sizeof(b), "%s%s %s", o, ou, col ? "NEAR" : "SENT");
        if (text_w(&font_small, b, 0) > 124 - 4 - text_w(&font_small, big_label, 0) - 6) snprintf(b, sizeof(b), "%s%s", o, ou);
        text_r(&font_small, 124, 52, b, C_DIM);
    }
    const char *bu = count_short(b, sizeof(b), col ? f->run : f->live);
    const lcd_font_t *nf = count_w(&font_big, b, bu) <= W - 8 ? &font_big : count_w(&font_num, b, bu) <= W - 8 ? &font_num : &font_head;
    int base = nf == &font_big ? 88 : nf == &font_num ? 84 : 80;
    ap_count(nf, 4, base, b, bu);

    // Bottom line: the mode and its channel ("WIFI CH 36"), captures or Bluetooth devices at the right. Whether the
    // right side fits is judged on the widest channel, so it never comes and goes as the channel changes.
    uint16_t mc = !strcmp(st->mode, "BT") ? C_BLUE : !strcmp(st->mode, "CAP") ? C_MAG : !strcmp(st->mode, "CSI") ? C_AMBER : C_CYAN;
    const char *mode = st->mode[0] ? st->mode : "--";
    text_ex(&font_mid, 4, 103, mode, 1, mc, mc, SOLID, 16);
    int used = 4 + text_w(&font_mid, mode, 1), worst = used;
    if (st->channel && strcmp(st->mode, "BT")) {
        text(&font_small, used + 5, 102, "CH", C_DIM);
        used += 5 + text_w(&font_small, "CH", 0);
        snprintf(b, sizeof(b), "%u", st->channel);
        text(&font_mid, used + 4, 103, b, C_WHITE);
        worst = used + 4 + text_w(&font_mid, "177", 0);
    }
    uint16_t rc = C_MAG;
    b[0] = 0;
    if (st->captures) snprintf(b, sizeof(b), "%lu CAP", (unsigned long)st->captures);
    else if (!strcmp(st->mode, "BT")) { snprintf(b, sizeof(b), "%lu DEV", (unsigned long)st->bt_devices); rc = C_BLUE; }
    if (b[0] && 124 - text_w(&font_small, b, 0) > worst + 6) text_r(&font_small, 124, 102, b, rc);

    const int sb = 125, top = 21;
    fill(2, sb + 1, W - 4, 1, C_EDGE);
    for (int i = 0; i < LCD_ACT_N; i++) {
        int x = i < 14 ? 3 + i * 3 : 49 + (i - 14) * 27 / 10;
        int h = f->act[i] * top / 250;
        if (i == f->cur) { if (h < 4) h = 4; vgrad(x, sb - h, 2, h, C_YEL, C_AMBER); }
        else if (f->act[i] < 6) fill(x, sb - 1, 2, 1, C_FAINT);
        else for (int y = 0; y < h; y++) fill(x, sb - 1 - y, 2, 1, mix(C_TEAL, C_MAG, y, top));
    }
}

static void mini_splash(const lcd_frame_t *f) {
    background();
    const int cx = W / 2, cy = 50;
    for (int r = 15; r <= 45; r += 15) circle(cx, cy, r, 1, C_TEAL, 12);
    fill_a(cx - 46, cy, 92, 1, C_TEAL, 8);
    fill_a(cx, cy - 46, 1, 92, C_TEAL, 8);
    sweep(cx, cy, 45, f->now - f->t0);
    circle(cx, cy, 3, 0, C_WHITE, 16);
    int hw = text_w(&font_title, "WARDRIVE", 1);
    text_ex(&font_title, (W - hw) / 2, 112, "WARDRIVE", 1, C_CYAN, C_MAG, HORIZ, 16);
    text(&font_small, (W - text_w(&font_small, f->version, 0)) / 2, 126, f->version, C_DIM);
}

static void mini_alert(const lcd_frame_t *f) {
    uint16_t c, dark;
    alert_colors(f, &c, &dark);
    bool lit = f->alert_on || f->alert_police;
    uint16_t fg = lit ? C_WHITE : c;
    if (lit) vgrad(0, s_y0, W, SH, mix(c, dark, s_y0, H), mix(c, dark, s_y0 + SH - 1, H));
    else { background(); vgrad(0, s_y0, W, SH, mix(dark, C_BLACK, s_y0, H), mix(dark, C_BLACK, s_y0 + SH - 1, H)); }
    rrect_line(2, 2, W - 4, H - 4, 9, 2, fg, 16);
    warn_triangle(W / 2, 12, 50, 28, fg, lit ? c : C_BLACK);
    const lcd_font_t *lf = text_w(&font_head, f->alert, 0) <= W - 10 ? &font_head :
                           text_w(&font_title, f->alert, 0) <= W - 10 ? &font_title : &font_small;
    text(lf, (W - text_w(lf, f->alert, 0)) / 2, 94, f->alert, fg);
    text_ex(&font_small, (W - text_w(&font_small, "ALERT", 1)) / 2, 114, "ALERT", 1, fg, fg, SOLID, 10);
}

// ── firmware update: a progress ring filling cyan → magenta, a dashed ring spinning around it, the percent inside ──
enum { RING_FILL, RING_DASH };
// The band r-th..r around (cx, cy), anti-aliased. Each pixel's angle (0 at 12 o'clock, clockwise, 0-255) picks it:
// RING_FILL draws angles below `upto` (256 = all), shaded c0 → c1 along the way; RING_DASH draws 16 dashes, turned by `upto`.
static void ring(int cx, int cy, int r, int th, int kind, int upto, uint16_t c0, uint16_t c1, int a) {
    if (cy + r < s_y0 || cy - r >= s_y0 + SH) return;
    int ya = cy - r - 1 < s_y0 ? s_y0 : cy - r - 1, yb = cy + r + 1 >= s_y0 + SH ? s_y0 + SH - 1 : cy + r + 1;
    int ro = r * 8 * r * 8, ri = (r - th) * 8 * (r - th) * 8;
    int out2 = (r + 2) * (r + 2), in2 = r - th - 2 > 0 ? (r - th - 2) * (r - th - 2) : 0;
    for (int j = ya; j <= yb; j++)
        for (int i = cx - r - 1; i <= cx + r + 1; i++) {
            int dx = i - cx, dy = j - cy, d2 = dx * dx + dy * dy;
            if (d2 > out2 || d2 < in2) continue;   // only the band gets sampled
            int p = (iatan2(dy, dx) - 192) & 255;
            if (kind == RING_FILL ? p >= upto : ((p + upto) & 15) >= 9) continue;
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int ex = i * 8 + sx * 2 + 1 - cx * 8 - 4, ey = j * 8 + sy * 2 + 1 - cy * 8 - 4, d = ex * ex + ey * ey;
                    if (d <= ro && d > ri) n++;
                }
            if (n) blend(i, j, kind == RING_FILL ? mix(c0, c1, p, 255) : c0, n * a / 16);
        }
}

// The ring's colors: cyan → magenta while it fills, green pulsing once it's installing.
// `gap`: outer dashes this far out from the ring; `inner`: the inner dashes too (only where the number clears them).
static void update_ring(const lcd_frame_t *f, int cx, int cy, int r, int th, int gap, bool inner) {
    const lcd_status_t *st = &f->st;
    int pulse = (int)((f->now / 60) % 16);
    if (pulse > 8) pulse = 16 - pulse;
    ring(cx, cy, r, th, RING_FILL, 256, C_EDGE, C_EDGE, 16);                            // the track
    if (st->ota_installing) {
        ring(cx, cy, r + th / 2 + 2, th + 4, RING_FILL, 256, C_GREEN, C_GREEN, 2 + pulse / 2);   // halo
        ring(cx, cy, r, th, RING_FILL, 256, C_GREEN, C_GREEN, 10 + pulse * 3 / 4);
    } else {
        int upto = st->ota_pct * 256 / 100;
        ring(cx, cy, r + 3, th + 6, RING_FILL, upto, C_TEAL, C_PLUM, 5);                 // soft glow under the fill
        ring(cx, cy, r, th, RING_FILL, upto, C_CYAN, C_MAG, 16);
    }
    // the spinner: 16 dashes turning one way, a thinner set the other way
    ring(cx, cy, r + th / 2 + gap, 3, RING_DASH, (int)(f->now / 14), st->ota_installing ? C_GREEN : C_CYAN, 0, 9);
    if (inner) ring(cx, cy, r - th - 5, 2, RING_DASH, (int)(256 - (f->now / 22) % 256), C_TEAL, 0, 10);
}

// The percent, big, centered on (cx, base) with a small "%" after it; a smaller font when it's wider than `maxw`.
static void update_pct(const lcd_frame_t *f, const lcd_font_t *nf, const lcd_font_t *small, const lcd_font_t *pf, int cx,
                       int base, int maxw) {
    char b[8];
    snprintf(b, sizeof(b), "%u", f->st.ota_pct);
    int pw = text_w(pf, "%", 0);
    if (text_w(nf, b, 1) + 3 + pw > maxw) { base -= (nf->ascent - small->ascent) / 2; nf = small; }
    int nw = text_w(nf, b, 1), x = cx - (nw + 3 + pw) / 2;
    uint16_t top = f->st.ota_installing ? C_GREEN : C_WHITE, bottom = f->st.ota_installing ? C_GREEN : C_CYAN;
    glow(nf, x, base, b, f->st.ota_installing ? C_GREEN : C_TEAL, 3);
    text_ex(nf, x, base, b, 1, top, bottom, VERT, 16);
    text(pf, x + nw + 3, base, "%", f->st.ota_installing ? C_GREEN : C_MAG);
}

// "KEEP POWER ON" breathing in amber (the one thing to know), "RESTARTS BY ITSELF" in green once installing.
static void update_warning(const lcd_frame_t *f, const lcd_font_t *wf, int base) {
    const char *s = f->st.ota_installing ? "RESTARTS BY ITSELF" : "KEEP POWER ON";
    if (text_w(wf, s, 1) > W - 6) s = f->st.ota_installing ? "RESTARTING" : "POWER ON";
    int breathe = (int)((f->now / 70) % 16);
    if (breathe > 8) breathe = 16 - breathe;
    uint16_t c = f->st.ota_installing ? C_GREEN : C_AMBER;
    int x = (W - text_w(wf, s, 1)) / 2;
    glow(wf, x, base, s, c, 1 + breathe / 4);
    text_ex(wf, x, base, s, 1, c, c, SOLID, 9 + breathe * 7 / 8);
}

// A data stream along a row: packets of four blocks running left to right while bytes arrive.
static void update_stream(const lcd_frame_t *f, int y, int x0, int x1, int bs) {
    if (f->st.ota_installing) return;
    int step = bs * 6, off = (int)((f->now / 10) % (uint32_t)step);
    for (int x = x0 - step + off; x < x1; x += step)
        for (int k = 0; k < 4; k++) {
            int xx = x + k * (bs + 2);
            if (xx >= x0 && xx + bs <= x1) fill_a(xx, y, bs, bs, k == 3 ? C_WHITE : C_CYAN, 3 + k * 4);
        }
}

static void update_scene(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    background();
    header(f);
    const char *title = st->ota_installing ? "INSTALLING" : "FIRMWARE UPDATE";
    int tw = text_w(&font_title, title, 2);
    text_ex(&font_title, (W - tw) / 2, 72, title, 2, C_CYAN, C_MAG, HORIZ, 16);
    const int cx = W / 2, cy = 168;
    update_ring(f, cx, cy, 70, 12, 8, true);
    update_pct(f, &font_big, &font_num, &font_head, cx, cy + 18, 2 * (70 - 12 - 5 - 2 - 6));   // clears the inner dashes
    char b[32];
    if (st->ota_installing) snprintf(b, sizeof(b), "CHECKING THE IMAGE");
    else snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)st->ota_kb, (unsigned long)st->ota_total_kb);
    text_ex(&font_small, (W - text_w(&font_small, b, 1)) / 2, 270, b, 1, C_DIM, C_DIM, SOLID, 16);
    update_warning(f, &font_title, 296);
    update_stream(f, 308, 12, W - 12, 4);
}

static void mini_update(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    background();
    vgrad(0, 0, W, 19, C_HEAD0, C_HEAD1);
    const char *title = st->ota_installing ? "INSTALLING" : "UPDATING";
    text_ex(&font_title, (W - text_w(&font_title, title, 1)) / 2, 15, title, 1, C_CYAN, C_MAG, HORIZ, 16);
    hgrad(0, 19, W, 1, C_CYAN, C_MAG);
    const int cx = W / 2, cy = 63;
    update_ring(f, cx, cy, 33, 7, 5, false);   // no inner dashes: the number fills the ring
    update_pct(f, &font_num, &font_mid, &font_small, cx, cy + 11, 2 * (33 - 7 - 3));
    update_warning(f, &font_small, 120);
    update_stream(f, 124, 8, W - 8, 2);
}

// ── offline runs going to the phone: a card, a phone, packets flowing between them, a progress bar, then SENT ──
// A line from (x0, y0) to (x1, y1) with round ends, `r` pixels either side, anti-aliased.
static void seg(int x0, int y0, int x1, int y1, int r, uint16_t c, int a) {
    int ya = (y0 < y1 ? y0 : y1) - r - 1, yb = (y0 > y1 ? y0 : y1) + r + 1;
    int xa = (x0 < x1 ? x0 : x1) - r - 1, xb = (x0 > x1 ? x0 : x1) + r + 1;
    if (ya < s_y0) ya = s_y0;
    if (yb > s_y0 + SH - 1) yb = s_y0 + SH - 1;
    int64_t dx = (x1 - x0) * 8, dy = (y1 - y0) * 8, l2 = dx * dx + dy * dy, rr = (int64_t)(r * 8 + 4) * (r * 8 + 4);
    for (int j = ya; j <= yb; j++)
        for (int i = xa; i <= xb; i++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int64_t px = i * 8 + sx * 2 + 1 - x0 * 8 - 4, py = j * 8 + sy * 2 + 1 - y0 * 8 - 4;
                    int64_t t = l2 ? px * dx + py * dy : 0;
                    if (t < 0) t = 0;
                    if (t > l2) t = l2;
                    int64_t qx = l2 ? px - dx * t / l2 : px, qy = l2 ? py - dy * t / l2 : py;
                    if (qx * qx + qy * qy <= rr) n++;
                }
            if (n) blend(i, j, c, n * a / 16);
        }
}

// A microSD card: a rounded rectangle with its top right corner cut off by `notch`.
static void card_shape(int x, int y, int w, int h, int notch, uint16_t c, int a) {
    int ya = y < s_y0 ? s_y0 : y, yb = y + h > s_y0 + SH ? s_y0 + SH : y + h;
    for (int j = ya; j < yb; j++)
        for (int i = x; i < x + w; i++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px = i * 8 + sx * 2 + 1, py = j * 8 + sy * 2 + 1;
                    if ((x + w) * 8 - px + py - y * 8 >= notch * 8) n++;
                }
            int rc = rr_cov(i, j, x, y, w, h, 2);
            if (rc < n) n = rc;
            if (n) blend(i, j, c, n * a / 16);
        }
}

static void sd_card_icon(const lcd_frame_t *f, int x, int y, int w, int h, bool big) {
    int notch = w / 4, t = big ? 2 : 1;
    card_shape(x - 2, y - 2, w + 4, h + 4, notch + 1, C_CYAN, 3);           // soft glow
    card_shape(x, y, w, h, notch, C_CYAN, 16);
    card_shape(x + t, y + t, w - 2 * t, h - 2 * t, notch - t + 1, C_PANEL, 16);
    vgrad(x + t + 1, y + h / 2, w - 2 * t - 2, h / 2 - t - 1, C_PANEL, C_HEAD0);
    int n = big ? 4 : 3, cw = big ? 4 : 2, x0 = x + t + 3, x1 = x + w - notch - 1;   // gold contacts beside the cut
    for (int k = 0; k < n; k++) fill(x0 + k * (x1 - x0) / n, y + t + 2, cw, h / 4, C_AMBER);
    const lcd_font_t *lf = big ? &font_title : &font_small;
    text(lf, x + (w - text_w(lf, "SD", 0)) / 2, y + h - (big ? 9 : 3), "SD", C_WHITE);
    bool blink = (f->now / 120) & 1;   // the card's activity light
    if (big) circle(x + w - 8, y + h - 7, 2, 0, C_MAG, blink ? 16 : 4);
}

// A phone: its screen fills with rows as the run arrives, the newest row lit.
static void phone_icon(int x, int y, int w, int h, int pct, bool big) {
    int r = big ? 7 : 3;
    rrect(x - 2, y - 2, w + 4, h + 4, r + 2, C_MAG, 3);
    rrect(x, y, w, h, r, C_MAG, 16);
    rrect(x + 2, y + 2, w - 4, h - 4, r - 1, C_BLACK, 16);
    int top = y + (big ? 9 : 4), bot = y + h - (big ? 9 : 4), sx = x + (big ? 4 : 3), sww = w - (big ? 8 : 6);
    vgrad(sx, top, sww, bot - top, C_NAVY, C_HEAD1);
    if (big) {
        rrect(x + w / 2 - 6, y + 4, 12, 2, 1, C_FAINT, 16);    // speaker
        rrect(x + w / 2 - 7, y + h - 6, 14, 2, 1, C_FAINT, 16); // home bar
    }
    int step = big ? 5 : 3, rows = (bot - top - 2) / step, lit = rows * pct / 100;
    for (int k = 0; k < lit; k++) {
        int len = sww - 4 - (int)((k * 7u + 3) % 5) * sww / 10;   // ragged, like lines of text
        fill_a(sx + 2, top + 2 + k * step, len, big ? 2 : 1, k == lit - 1 ? C_WHITE : C_CYAN, k == lit - 1 ? 16 : 9);
    }
}

// Packets running left to right between the card and the phone, shading cyan → magenta along the way; they fade in and
// out at the ends. `go`: still sending (packets move), else the lane rests.
static void packet_stream(const lcd_frame_t *f, int x0, int x1, int cy, int bs, bool go) {
    for (int x = x0; x < x1; x += 4) blend(x, cy, C_FAINT, 12);
    int h = bs / 2 + 2;   // a chevron at the phone's end
    seg(x1 + 2 - h, cy - h, x1 + 2, cy, 0, go ? C_MAG : C_FAINT, 16);
    seg(x1 + 2 - h, cy + h, x1 + 2, cy, 0, go ? C_MAG : C_FAINT, 16);
    x1 -= h + 2;
    if (!go) return;
    // ~30 px/s: the panel redraws every ~200-300 ms, and packets moving more than half a step per frame read as running
    // backward (phone to card), as they did at 125 px/s (owner 2026-11-03).
    int step = bs * 5 + 6, off = (int)((f->now / 32) % (uint32_t)step), fade = bs * 2;
    for (int x = x0 - step + off; x < x1; x += step)
        for (int k = 0; k < 3; k++) {
            int xx = x + k * (bs + 2);
            if (xx < x0 || xx + bs > x1) continue;
            int edge = xx - x0 < x1 - xx - bs ? xx - x0 : x1 - xx - bs, a = 6 + k * 5;
            if (edge < fade) a = a * edge / fade;
            uint16_t c = k == 2 ? C_WHITE : mix(C_CYAN, C_MAG, xx - x0, x1 - x0);
            rrect(xx, cy - bs / 2, bs, bs, bs / 3, c, a);
        }
}

// A right arrow (→) centered on (cx, cy).
static void arrow(int cx, int cy, int len, uint16_t c) {
    int h = len / 3;
    seg(cx - len / 2, cy, cx + len / 2, cy, 0, c, 16);
    seg(cx + len / 2 - h, cy - h, cx + len / 2, cy, 0, c, 16);
    seg(cx + len / 2 - h, cy + h, cx + len / 2, cy, 0, c, 16);
}

// "SD → PHONE" centered on `cx`.
static void sd_to_phone(const lcd_font_t *lf, int cx, int base, int alen, uint16_t c) {
    int gap = 5, w = text_w(lf, "SD", 1) + gap + alen + gap + text_w(lf, "PHONE", 1), x = cx - w / 2;
    text_ex(lf, x, base, "SD", 1, c, c, SOLID, 16);
    x += text_w(lf, "SD", 1) + gap;
    arrow(x + alen / 2, base - lf->ascent * 4 / 10, alen, c);
    text_ex(lf, x + alen + gap, base, "PHONE", 1, c, c, SOLID, 16);
}

/// KB with thousands separators: 0.4, 6.1, 412, 1,280 (one decimal under 10 KB).
static void kb_str(char *b, size_t n, uint32_t bytes) {
    uint32_t kb = bytes / 1024;
    if (kb < 10) snprintf(b, n, "%lu.%lu", (unsigned long)kb, (unsigned long)(bytes % 1024 * 10 / 1024));
    else if (kb < 1000) snprintf(b, n, "%lu", (unsigned long)kb);
    else snprintf(b, n, "%lu,%03lu", (unsigned long)(kb / 1000), (unsigned long)(kb % 1000));
}

// Parts in a row with a dot (the "·") between them, from x; returns the width. `draw` = false only measures.
static int dotted(const lcd_font_t *lf, int x, int base, const char *const *parts, const uint16_t *cols, int n, bool draw) {
    int gap = lf == &font_small ? 11 : 13, w = 0;
    for (int i = 0; i < n; i++) {
        if (!parts[i] || !parts[i][0]) continue;
        if (w) {
            if (draw) circle(x + w + gap / 2 + (lf == &font_small), base - lf->ascent * 4 / 10, 2, 0, C_DIM, 16);   // mono glyphs fill their cell
            w += gap;
        }
        if (draw) text(lf, x + w, base, parts[i], cols[i]);
        w += text_w(lf, parts[i], 0);
    }
    return w;
}
// The first of `n` alternatives (`per` parts each) that fits `maxw`, centered on cx.
static void dotted_fit(const lcd_font_t *lf, int cx, int base, int maxw, const char *const *alts, const uint16_t *cols,
                       int n, int per) {
    for (int k = 0; k < n; k++) {
        const char *const *p = alts + k * per;
        int w = dotted(lf, 0, base, p, cols, per, false);
        if (w <= maxw || k == n - 1) { dotted(lf, cx - w / 2, base, p, cols, per, true); return; }
    }
}

// The big bar: cyan → magenta fill with a gloss on top, a glint running through it and a bright leading edge, the
// percent inside.
static void sd_bar(const lcd_frame_t *f, int x, int y, int w, int h, int pct, const lcd_font_t *pf) {
    int r = h / 2;
    rrect(x, y, w, h, r, C_PANEL, 16);
    rrect_line(x, y, w, h, r, 1, C_EDGE, 16);
    int fw = (w - 4) * pct / 100;
    if (fw > 0) {
        int fx = x + 2, fy = y + 2, fh = h - 4, fr = fh / 2;
        if (fw < 2 * fr) fw = 2 * fr;
        int ya = fy < s_y0 ? s_y0 : fy, yb = fy + fh > s_y0 + SH ? s_y0 + SH : fy + fh;
        int gx = fx + (int)((f->now / 3) % (uint32_t)(fw + 60)) - 30;
        for (int j = ya; j < yb; j++)
            for (int i = fx; i < fx + fw; i++) {
                int cov = rr_cov(i, j, fx, fy, fw, fh, fr);
                if (!cov) continue;
                uint16_t c = mix(C_CYAN, C_MAG, i - fx, w - 5);
                if (j < fy + fh / 3) c = mix(c, C_WHITE, 5, 16);                      // gloss
                int d = i - gx < 0 ? gx - i : i - gx;
                if (d < 30 && pct < 100) c = mix(c, C_WHITE, (30 - d) * 7 / 30, 16);   // the glint
                blend(i, j, c, cov);
            }
    }
    char b[16];
    snprintf(b, sizeof(b), "%d%%", pct);
    int tx = x + (w - text_w(pf, b, 1)) / 2, base = y + (h + pf->ascent * 3 / 4) / 2;
    glow(pf, tx, base, b, C_BLACK, 7);
    text_ex(pf, tx, base, b, 1, C_WHITE, C_WHITE, SOLID, 16);
}

// One dot a run: green taken, the current one pulsing, the rest waiting (only when they fit).
static void run_dots(const lcd_frame_t *f, int cx, int cy, int r, int sp) {
    const lcd_status_t *st = &f->st;
    int n = st->sd_runs;
    if (n < 2 || n * sp > W - 16) return;
    int x0 = cx - (n - 1) * sp / 2, pulse = (int)((f->now / 60) % 16);
    if (pulse > 8) pulse = 16 - pulse;
    for (int k = 0; k < n; k++) {
        int x = x0 + k * sp;
        if (k + 1 < st->sd_run) circle(x, cy, r, 0, C_GREEN, 16);
        else if (k + 1 == st->sd_run) { circle(x, cy, r + 2, 0, C_CYAN, 2 + pulse / 2); circle(x, cy, r, 0, C_CYAN, 16); }
        else circle(x, cy, r, 1, C_FAINT, 16);
    }
}

static int sd_pct(const lcd_status_t *st) {
    return st->sd_size ? (int)((uint64_t)(st->sd_bytes > st->sd_size ? st->sd_size : st->sd_bytes) * 100 / st->sd_size) : 0;
}
static bool sd_all_read(const lcd_status_t *st) { return st->sd_size && st->sd_bytes >= st->sd_size; }

// The run line ("RUN 2 OF 3 · run_12.csv") and the bytes line ("412 / 1,280 KB · 6.1 KB/s"), shortened to fit.
static void sd_lines(const lcd_frame_t *f, const lcd_font_t *rf, int rbase, int kbase, int maxw) {
    const lcd_status_t *st = &f->st;
    char run[20], run_s[12], a[16], b[16], kb[40], kb_s[40], rate[24];
    snprintf(run, sizeof(run), "RUN %u OF %u", st->sd_run, st->sd_runs);
    snprintf(run_s, sizeof(run_s), "%u/%u", st->sd_run, st->sd_runs);
    const char *alts[] = { run, st->sd_file, run_s, st->sd_file, run, "" };
    const uint16_t rc[] = { C_WHITE, C_CYAN };
    dotted_fit(rf, W / 2, rbase, maxw, alts, rc, 3, 2);
    kb_str(a, sizeof(a), st->sd_bytes > st->sd_size ? st->sd_size : st->sd_bytes);
    kb_str(b, sizeof(b), st->sd_size);
    snprintf(kb, sizeof(kb), "%s / %s KB", a, b);
    snprintf(kb_s, sizeof(kb_s), "%s/%s KB", a, b);
    bool done = sd_all_read(st);   // every byte read: the phone is saving it
    if (done) snprintf(rate, sizeof(rate), "SAVING");
    else if (st->sd_rate) {
        char r[16];
        kb_str(r, sizeof(r), st->sd_rate);
        snprintf(rate, sizeof(rate), "%s KB/s", r);
    } else rate[0] = 0;
    const char *kalts[] = { kb, rate, kb_s, rate, kb_s, "" };
    const uint16_t kc[] = { C_DIM, done ? C_GREEN : C_CYAN };
    dotted_fit(&font_small, W / 2, kbase, maxw, kalts, kc, 3, 2);
}

static void sd_send_scene(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    int pct = sd_pct(st);
    background();
    header(f);
    int tw = text_w(&font_title, "SENDING RUNS", 2);
    glow(&font_title, (W - tw) / 2, 72, "SENDING RUNS", C_TEAL, 2);
    text_ex(&font_title, (W - tw) / 2, 72, "SENDING RUNS", 2, C_CYAN, C_MAG, HORIZ, 16);
    sd_to_phone(&font_small, W / 2, 92, 16, C_DIM);
    const int cy = 142;
    sd_card_icon(f, 22, cy - 30, 46, 60, true);
    phone_icon(180, cy - 36, 40, 72, pct, true);
    packet_stream(f, 78, 172, cy, 8, !sd_all_read(st));
    sd_lines(f, &font_mid, 208, 268, W - 16);
    sd_bar(f, 12, 220, W - 24, 28, pct, &font_title);
    run_dots(f, W / 2, 290, 4, 16);
    text_r(&font_small, 234, 314, f->version, C_FAINT);
}

static void mini_sd_send(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    int pct = sd_pct(st);
    background();
    vgrad(0, 0, W, 19, C_HEAD0, C_HEAD1);
    const char *title = text_w(&font_title, "SENDING RUNS", 0) <= W - 6 ? "SENDING RUNS" : "SENDING";
    text_ex(&font_title, (W - text_w(&font_title, title, 0)) / 2, 15, title, 0, C_CYAN, C_MAG, HORIZ, 16);
    hgrad(0, 19, W, 1, C_CYAN, C_MAG);
    sd_to_phone(&font_small, W / 2, 33, 12, C_DIM);
    const int cy = 54;
    sd_card_icon(f, 6, cy - 13, 21, 26, false);
    phone_icon(104, cy - 15, 18, 30, pct, false);
    packet_stream(f, 31, 100, cy, 4, !sd_all_read(st));
    sd_lines(f, &font_small, 84, 115, W - 8);
    sd_bar(f, 4, 89, W - 8, 15, pct, &font_small);
    run_dots(f, W / 2, 123, 2, 9);
}

// A check mark in a ring that draws itself around once as the screen comes up, a ripple going out.
static void sent_check(const lcd_frame_t *f, int cx, int cy, int r, int th) {
    uint32_t age = f->st.sd_sent_ms;
    int upto = age >= 400 ? 256 : (int)(age * 256 / 400);
    int span = r / 2 + 8, rip = r + 4 + (int)((age / 8) % (uint32_t)span), ra = 6 - (rip - r - 4) * 6 / span;
    if (ra > 0) circle(cx, cy, rip, 2, C_GREEN, ra);
    circle(cx, cy, r - th, 0, C_GREEN, 3);
    ring(cx, cy, r + 3, th + 6, RING_FILL, upto, C_GREEN, C_GREEN, 4);
    ring(cx, cy, r, th, RING_FILL, upto, C_GREEN, C_GREEN, 16);
    int s = r * 10 / 22, k = s / 5 > 1 ? s / 5 : 1;   // the ✓
    seg(cx - s, cy + s / 8, cx - s / 3, cy + s * 3 / 4, k + 2, C_TEAL, 6);
    seg(cx - s / 3, cy + s * 3 / 4, cx + s, cy - s * 2 / 3, k + 2, C_TEAL, 6);
    seg(cx - s, cy + s / 8, cx - s / 3, cy + s * 3 / 4, k, C_WHITE, 16);
    seg(cx - s / 3, cy + s * 3 / 4, cx + s, cy - s * 2 / 3, k, C_WHITE, 16);
}

// "3 RUNS · 1,280 KB" centered on the screen.
static void sent_summary(const lcd_frame_t *f, int base) {
    const lcd_status_t *st = &f->st;
    char runs[16], kb[16], kbu[20];
    snprintf(runs, sizeof(runs), "%u RUN%s", st->sd_runs, st->sd_runs == 1 ? "" : "S");
    kb_str(kb, sizeof(kb), st->sd_sent_bytes);
    snprintf(kbu, sizeof(kbu), "%s KB", kb);
    const char *parts[] = { runs, kbu };
    const uint16_t cols[] = { C_WHITE, C_DIM };
    int w = dotted(&font_small, 0, base, parts, cols, 2, false);
    dotted(&font_small, (W - w) / 2, base, parts, cols, 2, true);
}

static void sd_sent_scene(const lcd_frame_t *f) {
    background();
    header(f);
    sent_check(f, W / 2, 140, 56, 6);
    int tw = text_w(&font_head, "SENT", 4);
    glow(&font_head, (W - tw) / 2, 246, "SENT", C_TEAL, 2);
    text_ex(&font_head, (W - tw) / 2, 246, "SENT", 4, C_GREEN, C_GREEN, SOLID, 16);
    sent_summary(f, 272);
    sd_to_phone(&font_small, W / 2, 294, 16, C_GREEN);
    text_r(&font_small, 234, 314, f->version, C_FAINT);
}

static void mini_sd_sent(const lcd_frame_t *f) {
    background();
    sent_check(f, W / 2, 46, 30, 4);
    int tw = text_w(&font_head, "SENT", 3);
    glow(&font_head, (W - tw) / 2, 103, "SENT", C_TEAL, 2);
    text_ex(&font_head, (W - tw) / 2, 103, "SENT", 3, C_GREEN, C_GREEN, SOLID, 16);
    sent_summary(f, 121);
}

// ── public ──
int lcd_act_index(uint8_t ch) {
    if (ch >= 1 && ch <= 14) return ch - 1;
    if (ch >= 36 && ch <= 64) return 14 + (ch - 36) / 4;            // 36..64: 8
    if (ch >= 100 && ch <= 144) return 22 + (ch - 100) / 4;         // 100..144: 12
    if (ch >= 149 && ch <= 177) return 34 + (ch - 149) / 4;         // 149..177: 8
    return -1;
}

void lcd_draw_strip(uint16_t *strip, int y0, const lcd_frame_t *f) {
    s_px = strip;
    s_y0 = y0;
    s_w = f->w;
    s_h = f->h;
    bool mini = s_w < 200;
    if (f->st.ota) mini ? mini_update(f) : update_scene(f);
    else if (f->alerting && is_toy(f->alert)) toy_alert(f);
    else if (f->alerting) mini ? mini_alert(f) : alert(f);
    else if (f->st.sd_sent) mini ? mini_sd_sent(f) : sd_sent_scene(f);
    else if (f->st.sd_send) mini ? mini_sd_send(f) : sd_send_scene(f);
    else if (f->now - f->t0 < 2200) mini ? mini_splash(f) : splash(f);
    else mini ? mini_dashboard(f) : dashboard(f);
}
