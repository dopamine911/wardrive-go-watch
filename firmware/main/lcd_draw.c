// WardriveESP — the color screens' scenes: boot splash (radar sweep), live dashboard, alert banner, firmware update,
// offline runs going to the phone, each in a 240x320 and a compact 128x128 layout; and on watch builds (LCD_WATCH) the
// watch's own 410x502 pages. Anti-aliased text from lcd_fonts.h (lcd_fonts_watch.h for the watch); everything is drawn
// per 16-row strip, clipped, straight into the wire buffer.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>
#include "lcd_draw.h"
#include "lcd_fonts.h"
#include "lcd_sprites.h"
#ifndef LCD_WATCH
#define LCD_WATCH 0   // set for builds that can run the watch's 410x502 screen (ESP32-S3), and for the bench preview
#endif
#if LCD_WATCH
#include "lcd_fonts_watch.h"
#endif

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
    if (2 * r > w || 2 * r > h) {   // corners overlapping: every pixel through the coverage test
        for (int j = ya; j < yb; j++)
            for (int i = x; i < x + w; i++) {
                int cov = rr_cov(i, j, x, y, w, h, r);
                if (cov) blend(i, j, c, cov * a / 16);
            }
        return;
    }
    // Only the corners need the coverage test; everything else is fully covered, so a straight span (same pixels as
    // blending each at full coverage, without the per-pixel test).
    for (int j = ya; j < yb; j++) {
        bool band = j < y + r || j >= y + h - r;
        int ia = band ? x + r : x, ib = band ? x + w - r : x + w;
        for (int i = x; i < ia; i++) { int cov = rr_cov(i, j, x, y, w, h, r); if (cov) blend(i, j, c, cov * a / 16); }
        if (a >= 16) fill(ia, j, ib - ia, 1, c);
        else for (int i = ia; i < ib; i++) blend(i, j, c, a);
        for (int i = ib; i < x + w; i++) { int cov = rr_cov(i, j, x, y, w, h, r); if (cov) blend(i, j, c, cov * a / 16); }
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

/// Rows on the card, short: 9999, 38.4K, 1.2M.
static void rows_short(char *b, size_t n, unsigned long rows) {
    if (rows < 10000) snprintf(b, n, "%lu", rows);
    else if (rows < 1000000) snprintf(b, n, "%lu.%luK", rows / 1000, (rows / 100) % 10);
    else snprintf(b, n, "%lu.%luM", rows / 1000000, (rows / 100000) % 10);
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
    if (sd) rows_short(b, sizeof(b), st->sd_rows);   // in the clock's place while saving: rows on the card so far
    else if (up < 3600) snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(up / 60), (unsigned long)(up % 60));
    else snprintf(b, sizeof(b), "%luh%02lu", (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
    int clock_x = 222 - text_w(&font_small, b, 0);
    text(&font_small, clock_x, 69, b, sd ? C_WHITE : C_DIM);
    const char *what = st->usb ? "USB" : st->ble ? "BLUETOOTH" : sd ? "SAVING TO SD" : "NO PHONE";
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

static void ap_block(const lcd_frame_t *f) {
    fill(10, 87, 3, 11, C_CYAN);
    text_ex(&font_small, 18, 97, "IN RANGE", 1, C_DIM, C_DIM, SOLID, 16);
    char b[24];
    snprintf(b, sizeof(b), "%lu TOTAL", (unsigned long)f->session);
    int tx = 230 - text_w(&font_small, b, 0);
    text(&font_small, tx, 97, b, C_DIM);
    if (f->fresh) {
        snprintf(b, sizeof(b), "+%lu", (unsigned long)f->fresh);
        int pw = text_w(&font_title, b, 1) + 12;
        if (tx - 6 - pw > 18 + text_w(&font_small, "IN RANGE", 1) + 4) {   // only when it fits between the two
            rrect(tx - 6 - pw, 85, pw, 16, 8, C_GREEN, 4);
            rrect_line(tx - 6 - pw, 85, pw, 16, 8, 1, C_GREEN, 14);
            text_ex(&font_title, tx - 6 - pw + 6, 98, b, 1, C_GREEN, C_GREEN, SOLID, 16);
        }
    }
    snprintf(b, sizeof(b), "%lu", (unsigned long)f->live);
    const lcd_font_t *nf = text_w(&font_huge, b, 2) <= W - 20 ? &font_huge : &font_big;
    int base = nf == &font_huge ? 158 : 152;
    glow(nf, 10, base, b, C_TEAL, 3);
    text_ex(nf, 10, base, b, 2, C_WHITE, C_CYAN, VERT, 16);
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
    text_r(&font_small, 234, 314, f->version, C_FAINT);
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
    text_ex(&font_title, 4, 15, "WARDRIVE", 0, C_CYAN, C_MAG, HORIZ, 16);
    hgrad(0, 19, W, 1, C_CYAN, C_MAG);
    uint16_t lc = link_color(st);   // who holds the board: green USB, cyan Bluetooth, magenta saving to SD, amber nobody
    bool linked = st->usb || st->ble;
    int pulse = linked ? 16 : (int)((f->now / 90) % 16);
    if (!linked && pulse > 8) pulse = 16 - pulse;
    circle(118, 9, 6, 0, lc, linked ? 4 : pulse / 2);
    circle(118, 9, 3, 0, lc, linked ? 16 : 8 + pulse);

    text(&font_small, 4, 32, "IN RANGE", C_DIM);
    if (f->fresh) snprintf(b, sizeof(b), "+%lu NEW", (unsigned long)f->fresh);
    else snprintf(b, sizeof(b), "%lu TOTAL", (unsigned long)f->session);
    if (text_w(&font_small, b, 0) > 124 - 64) snprintf(b, sizeof(b), "%lu", (unsigned long)(f->fresh ? f->fresh : f->session));
    text_r(&font_small, 124, 32, b, f->fresh ? C_GREEN : C_DIM);
    snprintf(b, sizeof(b), "%lu", (unsigned long)f->live);
    const lcd_font_t *nf = text_w(&font_big, b, 1) <= W - 8 ? &font_big : text_w(&font_num, b, 1) <= W - 8 ? &font_num : &font_head;
    int base = nf == &font_big ? 70 : nf == &font_num ? 66 : 62;
    glow(nf, 4, base, b, C_TEAL, 3);
    text_ex(nf, 4, base, b, 1, C_WHITE, C_CYAN, VERT, 16);

    uint16_t mc = !strcmp(st->mode, "BT") ? C_BLUE : !strcmp(st->mode, "CAP") ? C_MAG : !strcmp(st->mode, "CSI") ? C_AMBER : C_CYAN;
    const char *mode = st->mode[0] ? st->mode : "--";
    text_ex(&font_mid, 4, 93, mode, 1, mc, mc, SOLID, 16);
    int used = 4 + text_w(&font_mid, mode, 1);
    if (st->channel) {
        snprintf(b, sizeof(b), "%u", st->channel);
        text(&font_mid, used + 6, 93, b, C_WHITE);
        used += 6 + text_w(&font_mid, b, 0);
    }
    uint16_t rc = C_MAG;
    b[0] = 0;
    if (st->captures) snprintf(b, sizeof(b), "%lu CAP", (unsigned long)st->captures);
    else if (!strcmp(st->mode, "BT")) { snprintf(b, sizeof(b), "%lu BT", (unsigned long)st->bt_devices); rc = C_BLUE; }
    else if (!linked && st->sd_log) { char r[12]; rows_short(r, sizeof(r), st->sd_rows); snprintf(b, sizeof(b), "SD %s", r); }
    if (b[0] && 124 - text_w(&font_small, b, 0) > used + 6) text_r(&font_small, 124, 92, b, rc);   // only when it fits

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
    int step = bs * 5 + 6, off = (int)((f->now / 8) % (uint32_t)step), fade = bs * 2;
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

#if LCD_WATCH
// ── the watch (410x502): black for the AMOLED, a faint terminal grid, big type for reading on a wrist ──
// The panel's visible area has rounded corners of about 110 px radius (bench 2026-11-03: of nested outlines 2 px from the
// edge, 90 px corners were cut off and 110 px ones whole). Anything within 110 px of a corner sits inside that curve.
#define W_CORNER_R 110
#define W_HEAD_X 46              // header: at the header's height the corner curve reaches ~37 px in
#define WM 24                    // side margin, everywhere at least ~55 px from the top and bottom edges
#define W_HEAD 60                // header height
#define W_DOTS_Y 490             // page dots
#define W_TILE_W 173
#define W_TILE_H 70
#define W_BTN_X WM
#define W_BTN_Y0 104
#define W_BTN_H 52
#define W_BTN_GAP 6
#define W_ROW_Y0 104             // networks page: first row, row pitch, row height
#define W_ROW_PITCH 70
#define W_ROW_H 64
#define W_ROW_M 16               // networks page rows: side margin (the last row ends ~54 px from the bottom)

static void w_background(void) {
    fill(0, s_y0, W, SH, C_BLACK);
    for (int y = 12; y < H; y += 24) {
        if (y < s_y0 || y >= s_y0 + SH) continue;
        for (int x = 12; x < W; x += 24) blend(x, y, C_FAINT, 5);
    }
}

/// `s` cut with "..." to fit `maxw` in font `fn`, into `out` (n >= 4).
static void w_fit(const lcd_font_t *fn, const char *s, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    for (size_t k = strlen(out); k > 0 && text_w(fn, out, 0) > maxw; k--)
        if (k + 3 < n) memcpy(out + k - 1, "...", 4);   // the first k - 1 characters, then "..."
}

// A halo in four directions: on black it reads like glow()'s twelve, at a third of the cost (the big digits are most of
// a watch frame's drawing).
static void w_glow(const lcd_font_t *f, int x, int base, const char *s, int sp, uint16_t c, int a) {
    static const int8_t D[][2] = { {-2,0},{2,0},{0,-2},{0,2} };
    for (size_t i = 0; i < sizeof(D) / sizeof(D[0]); i++) text_ex(f, x + D[i][0], base + D[i][1], s, sp, c, c, SOLID, a);
}

static void w_header(const lcd_frame_t *f) {
    fill(0, 0, W, W_HEAD, C_BLACK);
    text_ex(&wfont_head, W_HEAD_X, 50, "WARDRIVE", 1, C_CYAN, C_MAG, HORIZ, 16);
    int pct = f->st.batt_pct;
    if (pct >= 0) {   // battery: a cell with its level, the percent beside it
        if (pct > 100) pct = 100;
        uint16_t c = pct > 40 ? C_GREEN : pct > 15 ? C_AMBER : C_RED;
        int bx = W - W_HEAD_X - 50, by = 26;
        rrect_line(bx, by, 46, 24, 5, 2, C_DIM, 16);
        rrect(bx + 46, by + 7, 4, 10, 2, C_DIM, 16);
        int lw = 38 * pct / 100;
        if (lw > 0) rrect(bx + 4, by + 4, lw, 16, 3, c, 14);
        char b[8];
        snprintf(b, sizeof(b), "%d%%", pct);
        text_r(&wfont_label, bx - 8, 46, b, c);
    }
    hgrad(0, W_HEAD - 3, W, 2, C_CYAN, C_MAG);
}

static void w_dots(const lcd_frame_t *f) {
    for (int p = 0; p < LCD_PAGES; p++) {
        int cx = W / 2 + (2 * p - (LCD_PAGES - 1)) * 12;   // centered whatever the page count
        if (p == f->page) circle(cx, W_DOTS_Y, 6, 0, C_CYAN, 16);
        else circle(cx, W_DOTS_Y, 5, 2, C_EDGE, 16);
    }
}

static uint16_t w_link_color(const lcd_status_t *st) { return st->usb ? C_GREEN : st->ble ? C_CYAN : C_AMBER; }

static void w_clock(char *b, size_t n, uint32_t s) {
    if (s < 3600) snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
}

static void w_status(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    uint16_t c = w_link_color(st);
    bool linked = st->usb || st->ble;
    rrect(WM, 68, W - 2 * WM, 46, 23, c, 3);
    rrect_line(WM, 68, W - 2 * WM, 46, 23, 2, c, 10);
    int pulse = linked ? 16 : (int)((f->now / 90) % 16);
    if (!linked && pulse > 8) pulse = 16 - pulse;
    circle(WM + 24, 91, 11, 0, c, linked ? 4 : pulse / 2);
    circle(WM + 24, 91, 6, 0, c, linked ? 16 : 8 + pulse);
    char clk[12];
    w_clock(clk, sizeof(clk), (f->now - f->t0) / 1000);
    int cx = W - WM - 18 - text_w(&wfont_label, clk, 0);
    text(&wfont_label, cx, 99, clk, C_DIM);
    const char *what = st->usb ? "USB" : st->ble ? "BLE" : "NO PHONE";
    int tx = WM + 44;
    text_ex(&wfont_body, tx, 101, what, 1, c, c, SOLID, 16);
    if (linked && st->holder[0]) {
        char h[24];
        int hx = tx + text_w(&wfont_body, what, 1) + 10;
        w_fit(&wfont_body, st->holder, cx - 12 - hx, h, sizeof(h));
        text(&wfont_body, hx, 101, h, C_WHITE);
    }
}

static void w_tile(int x, int y, const char *label, const char *value, const char *unit, uint16_t accent, uint16_t vc) {
    rrect(x, y, W_TILE_W, W_TILE_H, 12, C_PANEL, 16);
    rrect_line(x, y, W_TILE_W, W_TILE_H, 12, 1, C_EDGE, 16);
    rrect(x + 6, y + 12, 4, W_TILE_H - 24, 2, accent, 16);
    text_ex(&wfont_label, x + 18, y + 25, label, 0, C_DIM, C_DIM, SOLID, 16);
    text(&wfont_value, x + 18, y + 62, value, vc);
    if (unit && *unit) text(&wfont_label, x + 24 + text_w(&wfont_value, value, 0), y + 60, unit, accent);
}

// 2.4 GHz channel activity under the count, the channel being scanned in yellow (the watch's radio has no 5 GHz).
static void w_spectrum(const lcd_frame_t *f, int base, int top) {
    const int n = 13, gap = 5, bw = (W - 2 * WM - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++) {
        int x = WM + i * (bw + gap), h = f->act[i] * top / 250;
        if (i == f->cur) {
            if (h < 6) h = 6;
            vgrad(x, base - h, bw, h, C_YEL, C_AMBER);
            fill_a(x - 1, base - h - 2, bw + 2, h + 2, C_YEL, 3);
        } else if (f->act[i] < 6) {
            fill(x, base - 3, bw, 3, C_FAINT);
        } else {
            for (int y = 0; y < h; y++) fill(x, base - 1 - y, bw, 1, mix(C_TEAL, C_MAG, y, top));
        }
    }
}

static const char *const W_DAYS[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const W_MONTHS[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

// The first page: a plain watch face. The time, the date, the battery, and one small counter for the scan mode.
static void w_watchface(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    fill(0, s_y0, W, SH, C_BLACK);
    char b[24];
    if (st->batt_pct >= 0) {   // the battery: percent and a small cell, centered at the top (inside the corner curve)
        int pct = st->batt_pct > 100 ? 100 : st->batt_pct;
        uint16_t c = pct > 40 ? C_DIM : pct > 15 ? C_AMBER : C_RED;
        snprintf(b, sizeof(b), "%d%%", pct);
        int tw = text_w(&wfont_label, b, 0), x0 = (W - (tw + 10 + 30)) / 2;
        text(&wfont_label, x0, 62, b, c);
        rrect_line(x0 + tw + 10, 46, 26, 16, 4, 2, c, 16);
        rrect(x0 + tw + 36, 50, 3, 8, 1, c, 16);
        int lw = 18 * pct / 100;
        if (lw > 0) rrect(x0 + tw + 14, 50, lw, 8, 2, c, 16);
    }
    // The time: 12-hour, no leading zero. The big fonts have digits and no colon or dash, so the colon is two dots, and
    // a clock that isn't set yet shows a short bar where each pair of digits goes.
    char hh[4], mm[4];
    unsigned h12 = st->wall_hour % 12 ? st->wall_hour % 12 : 12;
    if (st->wall_ok) { snprintf(hh, sizeof(hh), "%u", h12); snprintf(mm, sizeof(mm), "%02u", st->wall_min); }
    else { strcpy(hh, "00"); strcpy(mm, "00"); }
    const int sp = 3, colon = 30, base = 262;
    int hw = text_w(&wfont_huge, hh, sp), mw = text_w(&wfont_huge, mm, sp);
    int x0 = (W - (hw + colon + mw)) / 2;
    uint16_t dot = st->wall_ok ? C_WHITE : C_FAINT;
    if (st->wall_ok) {
        text_ex(&wfont_huge, x0, base, hh, sp, C_WHITE, C_WHITE, SOLID, 16);
        text_ex(&wfont_huge, x0 + hw + colon, base, mm, sp, C_WHITE, C_WHITE, SOLID, 16);
    } else {
        rrect(x0 + 8, base - 40, hw - 16, 8, 4, C_FAINT, 16);
        rrect(x0 + hw + colon + 8, base - 40, mw - 16, 8, 4, C_FAINT, 16);
    }
    circle(x0 + hw + colon / 2, base - 52, 5, 0, dot, 16);
    circle(x0 + hw + colon / 2, base - 22, 5, 0, dot, 16);
    if (st->wall_ok) {
        snprintf(b, sizeof(b), "%s %s %u", W_DAYS[st->wall_wday % 7], W_MONTHS[(st->wall_mon + 11) % 12], st->wall_mday);
        text_ex(&wfont_body, (W - text_w(&wfont_body, b, 2)) / 2, 322, b, 2, C_DIM, C_DIM, SOLID, 16);
        const char *ap = st->wall_hour >= 12 ? "PM" : "AM";
        text(&wfont_label, (W - text_w(&wfont_label, ap, 0)) / 2, 358, ap, C_FAINT);
    } else {
        const char *a = "NO TIME YET";
        text_ex(&wfont_body, (W - text_w(&wfont_body, a, 2)) / 2, 322, a, 2, C_FAINT, C_FAINT, SOLID, 16);
    }
    // One small counter for the scan mode: the number the dashboard shows large (APs in range, or BLE devices).
    const char *mode = !strcmp(st->mode, "BT") ? "BT" : !strcmp(st->mode, "CAP") ? "CAP" : "WI-FI";
    if (st->scanning) snprintf(b, sizeof(b), "%s  %lu", mode, (unsigned long)(!strcmp(st->mode, "BT") ? st->bt_devices : f->live));
    else snprintf(b, sizeof(b), "%s  --", mode);
    text(&wfont_label, (W - text_w(&wfont_label, b, 0)) / 2, 440, b, st->scanning ? C_DIM : C_FAINT);
}

static void w_dashboard(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    w_background();
    w_header(f);
    w_status(f);
    // The big number: APs in range (Wi-Fi scan, capture), or Bluetooth devices heard in the last whole 10 s window.
    bool bt = !strcmp(st->mode, "BT"), cap = !strcmp(st->mode, "CAP");
    fill(WM, 132, 4, 20, bt ? C_BLUE : C_CYAN);
    text_ex(&wfont_label, WM + 12, 150, bt ? "BLE DEVICES LAST 10 S" : "IN RANGE NOW", 1, C_DIM, C_DIM, SOLID, 16);
    char b[24];
    if (!bt && f->fresh) {   // "+N new in the last 10 s"
        snprintf(b, sizeof(b), "+%lu", (unsigned long)f->fresh);
        int pw = text_w(&wfont_title, b, 1) + 20;
        rrect(W - WM - pw, 128, pw, 28, 14, C_GREEN, 4);
        rrect_line(W - WM - pw, 128, pw, 28, 14, 2, C_GREEN, 14);
        text_ex(&wfont_title, W - WM - pw + 10, 151, b, 1, C_GREEN, C_GREEN, SOLID, 16);
    }
    snprintf(b, sizeof(b), "%lu", (unsigned long)(bt ? st->bt_devices : f->live));
    const lcd_font_t *nf = text_w(&wfont_huge, b, 3) <= W - 2 * WM ? &wfont_huge : &wfont_big;
    int base = nf == &wfont_huge ? 248 : 236;
    w_glow(nf, WM, base, b, 3, bt ? C_BLUE : C_TEAL, 6);
    text_ex(nf, WM, base, b, 3, C_WHITE, bt ? C_BLUE : C_CYAN, VERT, 16);
    if (!bt) w_spectrum(f, 292, 30);   // Wi-Fi channels; Bluetooth has none to show
    // Counts this watch measured itself (the app sends none back), for the mode the board is in, and the scan.
    const int x1 = WM, x2 = W - WM - W_TILE_W, y1 = 300, y2 = 376;
    if (bt) {
        snprintf(b, sizeof(b), "%lu", (unsigned long)st->bt_now);
        w_tile(x1, y1, "THIS WINDOW", b, NULL, C_BLUE, C_WHITE);
        snprintf(b, sizeof(b), "%lu", (unsigned long)st->bt_windows);
        w_tile(x2, y1, "WINDOWS", b, NULL, C_BLUE, C_WHITE);
        int best = -128;
        for (int i = 0; i < st->nets_n; i++) if (st->nets[i].rssi > best) best = st->nets[i].rssi;
        if (st->nets_n) snprintf(b, sizeof(b), "%d", best);
        else snprintf(b, sizeof(b), "--");
        w_tile(x1, y2, "STRONGEST", b, st->nets_n ? "dBm" : NULL, C_YEL, C_WHITE);
    } else if (cap) {
        snprintf(b, sizeof(b), "%lu", (unsigned long)st->cap_hs);
        w_tile(x1, y1, "HANDSHAKES", b, NULL, C_MAG, C_WHITE);
        snprintf(b, sizeof(b), "%lu", (unsigned long)st->cap_pmkid);
        w_tile(x2, y1, "PMKIDS", b, NULL, C_MAG, C_WHITE);
        snprintf(b, sizeof(b), "%lu", (unsigned long)st->cap_clients);
        w_tile(x1, y2, "CLIENTS", b, NULL, C_GREEN, C_WHITE);
    } else {
        snprintf(b, sizeof(b), "%lu", (unsigned long)f->session);
        w_tile(x1, y1, "SINCE BOOT", b, NULL, C_CYAN, C_WHITE);
        if (st->usb || st->ble) snprintf(b, sizeof(b), "%lu", (unsigned long)st->aps_link);
        else snprintf(b, sizeof(b), "--");
        w_tile(x2, y1, "THIS SESSION", b, NULL, C_GREEN, C_WHITE);
        if (st->channel) snprintf(b, sizeof(b), "%u", st->channel);
        else snprintf(b, sizeof(b), "--");
        w_tile(x1, y2, "CHANNEL", b, st->channel ? "2.4 GHz" : NULL, C_YEL, C_WHITE);
    }
    bool linked = st->usb || st->ble;   // linked but not listening: the phone's app isn't open (or hasn't subscribed)
    const char *scan = st->scanning ? "ON" : st->can_scan ? "PAUSED" : linked ? "WAITING" : "OFF";
    uint16_t sc = st->scanning ? C_GREEN : st->can_scan || linked ? C_AMBER : C_DIM;
    char lab[20];
    snprintf(lab, sizeof(lab), "SCAN %s", st->mode[0] ? st->mode : "");
    w_tile(x2, y2, lab, scan, NULL, sc, sc);
    if (st->scanning) {   // activity: a dot running round the scan tile's corner
        int ph = (int)((f->now / 120) % 8);
        for (int k = 0; k < 3; k++) circle(x2 + W_TILE_W - 22 + k * 7, y2 + 54, 3, 0, C_GREEN, ph % 3 == k ? 16 : 4);
    }
    text_ex(&wfont_label, (W - text_w(&wfont_label, "COUNTED ON THIS WATCH", 1)) / 2, 472, "COUNTED ON THIS WATCH", 1,
            C_FAINT, C_FAINT, SOLID, 16);
    w_dots(f);
}

static void w_title(const char *title, const char *right) {
    text_ex(&wfont_title, WM, 92, title, 2, C_CYAN, C_MAG, HORIZ, 16);
    if (right && *right) text_r(&wfont_label, W - WM, 92, right, C_DIM);
}

static uint16_t w_auth_color(const char *auth) {
    if (!auth || !strcmp(auth, "OPEN")) return C_RED;
    if (!strcmp(auth, "WEP") || !strcmp(auth, "WPA")) return C_AMBER;
    if (!strcmp(auth, "UNKNOWN")) return C_DIM;
    return C_GREEN;
}

static void w_networks(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    bool bt = !strcmp(st->mode, "BT");   // Bluetooth mode lists devices; Wi-Fi and capture list APs
    w_background();
    w_header(f);
    char b[40];
    int first = f->net_first < st->nets_n ? f->net_first : 0;
    int last = first + LCD_NETS_ROWS < st->nets_n ? first + LCD_NETS_ROWS : st->nets_n;
    if (st->nets_n > LCD_NETS_ROWS) snprintf(b, sizeof(b), "%d-%d OF %u", first + 1, last, st->nets_n);
    else snprintf(b, sizeof(b), "%u RECENT", st->nets_n);
    w_title(bt ? "DEVICES" : "NETWORKS", st->nets_n ? b : "");
    if (!st->nets_n) {
        const char *a = bt ? "NO DEVICES YET" : "NO NETWORKS YET";
        const char *c = (st->usb || st->ble) ? "scanning..." : "shown once Wardrive Go links";
        text_ex(&wfont_title, (W - text_w(&wfont_title, a, 2)) / 2, 260, a, 2, C_DIM, C_DIM, SOLID, 16);
        text(&wfont_label, (W - text_w(&wfont_label, c, 0)) / 2, 296, c, C_FAINT);
        w_dots(f);
        return;
    }
    for (int i = first; i < last; i++) {
        const netlog_rec_t *r = &st->nets[i];
        int y = W_ROW_Y0 + (i - first) * W_ROW_PITCH;
        if (y + W_ROW_H <= s_y0 || y >= s_y0 + SH) continue;
        uint16_t ac = bt ? C_BLUE : w_auth_color(r->auth);
        const int rl = W_ROW_M + 14, rr = W - W_ROW_M - 12;
        rrect(W_ROW_M, y, W - 2 * W_ROW_M, W_ROW_H, 10, C_PANEL, 16);
        rrect(W_ROW_M + 5, y + 10, 4, W_ROW_H - 20, 2, ac, 16);
        // RSSI on the right of the first line, colored by strength
        uint16_t rc = r->rssi > -60 ? C_GREEN : r->rssi > -75 ? C_YEL : C_AMBER;
        snprintf(b, sizeof(b), "%d", r->rssi);
        int dbw = text_w(&wfont_label, "dBm", 0), rx = rr - dbw - 4 - text_w(&wfont_body, b, 0);
        text(&wfont_body, rx, y + 30, b, rc);
        text(&wfont_label, rr - dbw, y + 29, "dBm", C_DIM);
        // SSID: printable ASCII as is, each other character (a UTF-8 sequence counts once) as '?' (the fonts are ASCII);
        // hidden ones say so
        char ssid[33];
        size_t k = 0;
        for (size_t j = 0; j < r->ssid_len && j < 32; j++) {
            uint8_t c = (uint8_t)r->ssid[j];
            if ((c & 0xC0) == 0x80) continue;   // UTF-8 continuation byte
            ssid[k++] = (c >= 32 && c < 127) ? (char)c : '?';
        }
        ssid[k] = '\0';
        char fit[40];
        w_fit(&wfont_body, r->ssid_len ? ssid : bt ? "<no name>" : "<hidden>", rx - 12 - rl, fit, sizeof(fit));
        text(&wfont_body, rl, y + 30, fit, r->ssid_len ? C_WHITE : C_DIM);
        // second line: the address, then (Wi-Fi) channel and security or (Bluetooth) the address type
        const uint8_t *m = r->bssid;
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        text(&wfont_label, rl, y + 56, b, C_DIM);
        const char *auth = r->auth ? (!strcmp(r->auth, "WPA2_WPA3") ? "WPA2/3" : r->auth) : "?";
        int aw = text_w(&wfont_label, auth, 0);
        text(&wfont_label, rr - aw, y + 56, auth, ac);
        if (bt) continue;
        snprintf(b, sizeof(b), "ch%u", r->ch);
        text(&wfont_label, rr - aw - 10 - text_w(&wfont_label, b, 0), y + 56, b, C_YEL);
    }
    if (st->nets_n > LCD_NETS_ROWS)
        text_ex(&wfont_label, (W - text_w(&wfont_label, "SWIPE UP / DOWN", 1)) / 2, 472, "SWIPE UP / DOWN", 1, C_FAINT,
                C_FAINT, SOLID, 16);
    w_dots(f);
}

int lcd_watch_button_at(int x, int y) {
    if (x < W_BTN_X || x >= LCD_W - W_BTN_X) return LCD_BTN_NONE;
    for (int b = 0; b < LCD_BTNS; b++) {
        int by = W_BTN_Y0 + b * (W_BTN_H + W_BTN_GAP);
        if (y >= by && y < by + W_BTN_H) return b;
    }
    return LCD_BTN_NONE;
}

static void w_button(const lcd_frame_t *f, int b, const char *label, const char *value, uint16_t vc, bool enabled) {
    int y = W_BTN_Y0 + b * (W_BTN_H + W_BTN_GAP), w = W - 2 * W_BTN_X;
    bool held = f->hold_btn == b;
    uint16_t edge = !enabled ? C_EDGE : held ? C_CYAN : C_EDGE;
    rrect(W_BTN_X, y, w, W_BTN_H, 14, C_PANEL, 16);
    if (held && enabled && f->hold_pct) rrect(W_BTN_X, y, w * f->hold_pct / 100, W_BTN_H, 14, C_CYAN, 5);
    rrect_line(W_BTN_X, y, w, W_BTN_H, 14, held ? 2 : 1, edge, 16);
    text_ex(&wfont_title, W_BTN_X + 18, y + 35, label, 1, enabled ? C_WHITE : C_DIM, enabled ? C_WHITE : C_DIM, SOLID, 16);
    text_r(&wfont_body, W - W_BTN_X - 18, y + 37, value, enabled ? vc : C_DIM);
}

static void w_info(int y, const char *label, const char *value) {
    text(&wfont_label, WM, y, label, C_DIM);
    char fit[40];
    w_fit(&wfont_label, value, W - 2 * WM - 120, fit, sizeof(fit));
    text_r(&wfont_label, W - WM, y, fit, C_WHITE);
}

static void w_controls(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    w_background();
    w_header(f);
    w_title("CONTROLS", "HOLD TO CHANGE");
    char b[40];
    const char *sv = st->scanning ? "RUNNING" : st->can_scan ? "PAUSED" : (st->usb || st->ble) ? "OPEN THE APP" : "NEEDS PHONE";
    w_button(f, LCD_BTN_SCAN, "SCAN", sv, st->scanning ? C_GREEN : C_AMBER, st->scanning || st->can_scan);
    const char *mv = !strcmp(st->mode, "BT") ? "BLUETOOTH" : !strcmp(st->mode, "CAP") ? "CAPTURE" : "WI-FI";
    uint16_t mc = !strcmp(st->mode, "BT") ? C_BLUE : !strcmp(st->mode, "CAP") ? C_MAG : C_CYAN;
    w_button(f, LCD_BTN_MODE, "MODE", mv, mc, st->can_scan);
    snprintf(b, sizeof(b), "%u%%", f->bright_pct);
    w_button(f, LCD_BTN_BRIGHT, "BRIGHTNESS", b, C_CYAN, true);
    snprintf(b, sizeof(b), "%u s", f->sleep_s);
    w_button(f, LCD_BTN_SLEEP, "SCREEN OFF", b, C_CYAN, true);
    hgrad(WM, 342, W - 2 * WM, 1, C_EDGE, C_EDGE);
    w_info(368, "BLE", st->ble_name[0] ? st->ble_name : "--");
    if (st->usb) snprintf(b, sizeof(b), "USB host");
    else if (st->ble) snprintf(b, sizeof(b), "%s  MTU %u", st->holder[0] ? st->holder : "phone", st->mtu);
    else snprintf(b, sizeof(b), "advertising");
    w_info(394, "LINK", b);
    if (st->batt_pct >= 0) snprintf(b, sizeof(b), "%d%%  %d.%02d V", st->batt_pct, st->batt_mv / 1000, st->batt_mv % 1000 / 10);
    else snprintf(b, sizeof(b), "--");
    w_info(420, "BATTERY", b);
    char up[12], ver[40];
    w_clock(up, sizeof(up), (f->now - f->t0) / 1000);
    snprintf(ver, sizeof(ver), "%s  up %s", f->version, up);   // one row: an uptime row would sit inside the corner curve
    w_info(446, "FIRMWARE", ver);
    w_dots(f);
}

static void w_alert(const lcd_frame_t *f) {
    uint16_t c, dark;
    alert_colors(f, &c, &dark);
    bool lit = f->alert_on || f->alert_police;
    uint16_t fg = lit ? C_WHITE : c;
    if (lit) vgrad(0, s_y0, W, SH, mix(c, dark, s_y0, H), mix(c, dark, s_y0 + SH - 1, H));
    else { w_background(); vgrad(0, s_y0, W, SH, mix(dark, C_BLACK, s_y0, H), mix(dark, C_BLACK, s_y0 + SH - 1, H)); }
    rrect_line(6, 6, W - 12, H - 12, W_CORNER_R - 4, 4, fg, 16);   // follows the panel's own corners
    warn_triangle(W / 2, 92, 150, 86, fg, lit ? c : C_BLACK);
    const lcd_font_t *lf = text_w(&wfont_head, f->alert, 2) <= W - 48 ? &wfont_head : &wfont_title;
    text_ex(lf, (W - text_w(lf, f->alert, 2)) / 2, 312, f->alert, 2, fg, fg, SOLID, 16);
    text_ex(&wfont_label, (W - text_w(&wfont_label, "ALERT FROM THE APP", 1)) / 2, 356, "ALERT FROM THE APP", 1, fg, fg,
            SOLID, 10);
}

static void w_update(const lcd_frame_t *f) {
    const lcd_status_t *st = &f->st;
    w_background();
    w_header(f);
    const char *title = st->ota_installing ? "INSTALLING" : "FIRMWARE UPDATE";
    text_ex(&wfont_title, (W - text_w(&wfont_title, title, 2)) / 2, 108, title, 2, C_CYAN, C_MAG, HORIZ, 16);
    const int cx = W / 2, cy = 252, r = 110, th = 16;
    update_ring(f, cx, cy, r, th, 10, true);
    update_pct(f, &wfont_huge, &wfont_big, &wfont_head, cx, cy + 34, 2 * (r - th - 5 - 2 - 6));
    char b[32];
    if (st->ota_installing) snprintf(b, sizeof(b), "CHECKING THE IMAGE");
    else snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)st->ota_kb, (unsigned long)st->ota_total_kb);
    text_ex(&wfont_label, (W - text_w(&wfont_label, b, 1)) / 2, 404, b, 1, C_DIM, C_DIM, SOLID, 16);
    update_warning(f, &wfont_title, 444);
    update_stream(f, 462, W_HEAD_X, W - W_HEAD_X, 6);
}

static void w_splash(const lcd_frame_t *f) {
    w_background();
    const int cx = W / 2, cy = 196;
    for (int r = 46; r <= 138; r += 46) circle(cx, cy, r, 2, C_TEAL, 12);
    fill_a(cx - 140, cy, 280, 1, C_TEAL, 8);
    fill_a(cx, cy - 140, 1, 280, C_TEAL, 8);
    sweep(cx, cy, 138, f->now - f->t0);
    circle(cx, cy, 6, 0, C_WHITE, 16);
    int hw = text_w(&wfont_head, "WARDRIVE", 4);
    w_glow(&wfont_head, (W - hw) / 2, 392, "WARDRIVE", 4, C_TEAL, 4);
    text_ex(&wfont_head, (W - hw) / 2, 392, "WARDRIVE", 4, C_CYAN, C_MAG, HORIZ, 16);
    int pw = text_w(&wfont_title, "ESP", 0) + 14;
    rrect((W - pw) / 2, 408, pw, 30, 7, C_CYAN, 16);
    text(&wfont_title, (W - pw) / 2 + 7, 432, "ESP", C_BLACK);
    char b[48];
    snprintf(b, sizeof(b), "%s  %s", f->board, f->version);
    text(&wfont_label, (W - text_w(&wfont_label, b, 0)) / 2, 476, b, C_DIM);
}

static void watch_scene(const lcd_frame_t *f) {
    if (f->st.ota) w_update(f);
    else if (f->alerting && is_toy(f->alert)) toy_alert(f);
    else if (f->alerting) w_alert(f);
    else if (f->now - f->t0 < 2200) w_splash(f);
    else if (f->page == LCD_PAGE_CLOCK) w_watchface(f);
    else if (f->page == LCD_PAGE_NETS) w_networks(f);
    else if (f->page == LCD_PAGE_CTRL) w_controls(f);
    else w_dashboard(f);
}
#endif

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
#if LCD_WATCH
    if (s_w > 300) { watch_scene(f); return; }
#endif
    bool mini = s_w < 200;
    if (f->st.ota) mini ? mini_update(f) : update_scene(f);
    else if (f->alerting && is_toy(f->alert)) toy_alert(f);
    else if (f->alerting) mini ? mini_alert(f) : alert(f);
    else if (f->st.sd_sent) mini ? mini_sd_sent(f) : sd_sent_scene(f);
    else if (f->st.sd_send) mini ? mini_sd_send(f) : sd_send_scene(f);
    else if (f->now - f->t0 < 2200) mini ? mini_splash(f) : splash(f);
    else mini ? mini_dashboard(f) : dashboard(f);
}
