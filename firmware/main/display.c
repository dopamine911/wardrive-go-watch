// WardriveESP — 128x32 SSD1306 status screen over I2C (400 kHz): boot splash, live dashboard, alert banners.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "display.h"

static const char *TAG = "WardriveESP";

#define W 128
#define H 32
#define ADDR 0x3C
#define CONTRAST_ON  0xCF
#define CONTRAST_DIM 0x08
#define IDLE_DIM_MS  30000      // nobody linked this long: dim
#define SHIFT_MS     180000     // burn-in care: move everything by a pixel every 3 minutes
#define FRAME_MS     250
#define ALERT_BLINK_MS 250

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t *s_fb, *s_sent;  // frame buffer and what the panel already shows (page layout, 8 rows per byte)
static TaskHandle_t s_task;
static display_fill_fn s_fill;
static const char *s_board = "", *s_version = "";
static volatile bool s_stealth;
static volatile uint32_t s_alert_since, s_alert_until;
static char s_alert_label[13];
static uint32_t s_t0;

// AP sightings: unique BSSIDs this session (open-addressed hash set), new ones per second, activity per channel.
#define SEEN_N 1024
static uint32_t *s_seen;
static volatile uint32_t s_unique, s_seen_used;
static volatile uint16_t s_new_slot[10];
static volatile uint8_t s_act[42];   // 14 × 2.4 GHz, then 28 × 5 GHz
static uint32_t s_session_at[10];    // the session AP count at the start of each of the last ten seconds

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Classic public-domain 5x7 font, ASCII 32-126, one byte per column, LSB at the top.
static const uint8_t FONT[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},
};

// ── panel I/O ──
static esp_err_t cmds(const uint8_t *c, size_t n) {
    uint8_t buf[32];
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[0] = 0x00;   // control byte: commands follow
    memcpy(buf + 1, c, n);
    return i2c_master_transmit(s_dev, buf, n + 1, 50);
}
static esp_err_t cmd1(uint8_t c) { return cmds(&c, 1); }

// Sends only the pages that changed since the last frame.
static void flush(void) {
    uint8_t buf[1 + W];
    for (int page = 0; page < H / 8; page++) {
        uint8_t *src = s_fb + page * W;
        if (!memcmp(src, s_sent + page * W, W)) continue;
        const uint8_t at[] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };
        if (cmds(at, sizeof(at)) != ESP_OK) return;
        buf[0] = 0x40;   // control byte: display data follows
        memcpy(buf + 1, src, W);
        if (i2c_master_transmit(s_dev, buf, sizeof(buf), 50) != ESP_OK) return;
        memcpy(s_sent + page * W, src, W);
    }
}

// ── drawing (with the burn-in offset) ──
static int s_ox, s_oy;
static void px(int x, int y, bool on) {
    x += s_ox; y += s_oy;
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint8_t *b = &s_fb[(y / 8) * W + x];
    if (on) *b |= (uint8_t)(1 << (y & 7)); else *b &= (uint8_t)~(1 << (y & 7));
}
static void fill(int x, int y, int w, int h, bool on) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, on);
}
// One character at scale s (5x7 glyph cell, 6 px advance per scale step). `on` false draws dark on light.
static void glyph(int x, int y, char c, int s, bool on) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = FONT[c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row)) fill(x + col * s, y + row * s, s, s, on);
}
static int text_w(const char *t, int s) { int n = (int)strlen(t); return n ? n * 6 * s - s : 0; }
static void text(int x, int y, const char *t, int s, bool on) {
    for (; *t; t++, x += 6 * s) glyph(x, y, *t, s, on);
}
static void text_c(int y, const char *t, int s, bool on) { text((W - text_w(t, s)) / 2, y, t, s, on); }

// ── AP sightings ──
static int act_index(uint8_t ch) {
    if (ch >= 1 && ch <= 14) return ch - 1;
    if (ch >= 36 && ch <= 64) return 14 + (ch - 36) / 4;            // 36..64: 8
    if (ch >= 100 && ch <= 144) return 22 + (ch - 100) / 4;         // 100..144: 12
    if (ch >= 149 && ch <= 177) return 34 + (ch - 149) / 4;         // 149..177: 8
    return -1;
}

void display_note_ap(const uint8_t *bssid, uint8_t channel) {
    if (!s_seen) return;
    int a = act_index(channel);
    if (a >= 0) s_act[a] = s_act[a] > 226 ? 250 : s_act[a] + 24;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ bssid[i]) * 16777619u;
    if (!h) h = 1;
    if (s_seen_used > SEEN_N * 3 / 4) { memset(s_seen, 0, SEEN_N * sizeof(uint32_t)); s_seen_used = 0; }   // full: start over
    for (uint32_t i = h % SEEN_N, n = 0; n < SEEN_N; i = (i + 1) % SEEN_N, n++) {
        if (s_seen[i] == h) return;
        if (!s_seen[i]) {
            s_seen[i] = h;
            s_seen_used++;
            s_unique++;
            s_new_slot[(now_ms() / 1000) % 10]++;
            return;
        }
    }
}

// ── screens (128x32: content stays inside x 0-126, y 0-30 so the 1-px burn-in shift never clips) ──
// Hand-drawn 11x16 bold digits for the AP count, one word per column, bit 0 at the top.
static const uint16_t BIG[10][11] = {
    {0x0FF0,0x3FFC,0x7FFE,0xE007,0xC003,0xC003,0xC003,0xE007,0x7FFE,0x3FFC,0x0FF0},
    {0x0000,0x0010,0xC018,0xC01C,0xC00E,0xC007,0xFFFF,0xFFFF,0xC000,0xC000,0x0000},
    {0xE00C,0xF00E,0xF807,0xDC03,0xCE03,0xC703,0xC383,0xC1C7,0xC0FE,0xC07C,0xC038},
    {0x3004,0x7006,0xE007,0xC003,0xC0C3,0xC0C3,0xC0C3,0xE0EF,0x79FE,0x3FBC,0x1F10},
    {0x0780,0x07C0,0x07E0,0x0670,0x0638,0x061C,0x060E,0x0607,0xFFFF,0xFFFF,0x0600},
    {0x30FF,0x70FF,0xE0C3,0xC063,0xC063,0xC063,0xC063,0xE0E3,0x79E3,0x3F83,0x1F00},
    {0x1FF0,0x3FFC,0x70DE,0xE067,0xC063,0xC063,0xC063,0xE0E3,0x79E7,0x3FC6,0x1F80},
    {0x0003,0x0003,0x0003,0xF803,0xFE03,0xFF83,0x07E3,0x01FB,0x007F,0x001F,0x0007},
    {0x1E00,0x3F3C,0x73FE,0xE1E7,0xC0C3,0xC0C3,0xC0C3,0xE1E7,0x73FE,0x3F3C,0x1E00},
    {0x00F8,0x31FC,0x73CE,0xE387,0xC303,0xC303,0xC303,0xE307,0xF98E,0x7FFC,0x0FF8},
};
#define BIG_ADV 13

static int big_number(int x, int y, uint32_t n) {
    char d[11];
    int len = snprintf(d, sizeof(d), "%lu", (unsigned long)n);
    for (int i = 0; i < len; i++, x += BIG_ADV)
        for (int col = 0; col < 11; col++)
            for (int row = 0; row < 16; row++)
                if (BIG[d[i] - '0'][col] & (1u << row)) px(x + col, y + row, true);
    return len * BIG_ADV - 2;
}

static void splash(uint32_t t) {
    memset(s_fb, 0, W * H / 8);
    text(0, 1, "WARDRIVE", 2, true);
    fill(98, 0, 28, 16, true);
    text(101, 5, "ESP", 1, false);
    text(0, 20, s_version, 1, true);
    text(126 - text_w(s_board, 1) > 40 ? 126 - text_w(s_board, 1) : 40, 20, s_board, 1, true);
    int sweep = (int)(t * 120 / 1800);   // a scan line runs across while it boots
    if (sweep < 120) fill(sweep, 29, 7, 2, true);
}

static void dashboard(const display_status_t *st, uint32_t now) {
    memset(s_fb, 0, W * H / 8);
    char buf[32];
    // row 1: who holds the board, uptime
    if (st->usb) snprintf(buf, sizeof(buf), "USB%s%.13s", st->holder[0] ? " " : "", st->holder);
    else if (st->ble) snprintf(buf, sizeof(buf), "BT%s%.13s", st->holder[0] ? " " : "", st->holder);
    else snprintf(buf, sizeof(buf), "waiting for a phone");
    uint32_t up = (now - s_t0) / 1000;
    char upb[12];
    if (up < 3600) snprintf(upb, sizeof(upb), "%lu:%02lu", (unsigned long)(up / 60), (unsigned long)(up % 60));
    else snprintf(upb, sizeof(upb), "%luh%02lu", (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
    int upx = 127 - text_w(upb, 1);
    buf[(upx - 4) / 6 < (int)sizeof(buf) ? (upx - 4) / 6 : (int)sizeof(buf) - 1] = '\0';   // never runs into the clock
    text(0, 0, buf, 1, true);
    text(upx, 0, upb, 1, true);

    // rows 2-3: APs in range now in big digits, then the "+N" ticker, mode/channel and captures beside it
    int right = big_number(0, 9, st->aps_live) + 6;
    if (right < 40) right = 40;
    uint32_t then = s_session_at[(now / 1000 + 1) % 10];   // "+N new": unique this session now minus ten seconds ago
    uint32_t fresh = st->aps_session > then ? st->aps_session - then : 0;
    if (fresh) snprintf(buf, sizeof(buf), "+%lu new", (unsigned long)fresh);
    else snprintf(buf, sizeof(buf), "APs");
    text(right, 9, buf, 1, true);
    if (st->captures) {
        snprintf(buf, sizeof(buf), "%lu cap", (unsigned long)st->captures);
        text(127 - text_w(buf, 1), 9, buf, 1, true);
    }
    if (!strcmp(st->mode, "BT")) snprintf(buf, sizeof(buf), "BT %lu dev", (unsigned long)st->bt_devices);
    else if (st->channel) snprintf(buf, sizeof(buf), "%s ch %u", st->mode, st->channel);
    else snprintf(buf, sizeof(buf), "%s", st->mode);
    text(right, 18, buf, 1, true);

    // row 4: channel activity, 2.4 GHz then 5 GHz, with a cursor under the channel being scanned
    int cur = act_index(st->channel);
    for (int i = 0; i < 42; i++) {
        int x = i < 14 ? i * 3 : 43 + (i - 14) * 3;
        int h = (s_act[i] + 15) / 16;   // one AP shows; a busy channel fills the bar
        if (h > 4) h = 4;
        if (h) fill(x, 30 - h - 1, 2, h, true);
        else px(x, 28, true);
        if (i == cur) fill(x, 30, 2, 1, true);
    }
}

// Firmware update: "FLASHING" over the big percent, a striped bar filling underneath with a bright gap running
// through what's done (it keeps moving while bytes arrive), "KEEP POWER" beside the number. Installing blinks.
static void update_screen(const display_status_t *st, uint32_t now) {
    memset(s_fb, 0, W * H / 8);
    char buf[24];
    bool blink = (now / 400) & 1;
    if (st->ota_installing) { if (!blink) text(0, 0, "INSTALLING", 1, true); }
    else text(0, 0, "FLASHING FW", 1, true);
    snprintf(buf, sizeof(buf), "%lu/%luK", (unsigned long)st->ota_kb, (unsigned long)st->ota_total_kb);
    if (!st->ota_installing && text_w(buf, 1) <= 127 - 72) text(127 - text_w(buf, 1), 0, buf, 1, true);
    int right = big_number(0, 9, st->ota_pct);
    text(right + 2, 9, "%", 1, true);
    if (st->ota_installing) { text(right + 14, 9, "RESTARTS", 1, true); text(right + 14, 17, "BY ITSELF", 1, true); }
    else { text(right + 14, 9, "KEEP", 1, true); text(right + 14, 17, "POWER ON", 1, true); }
    // the bar: outline, diagonal-striped fill, a 3-px gap sweeping through the filled part
    fill(0, 26, 127, 1, true);
    fill(0, 30, 127, 1, true);
    px(0, 27, true); px(0, 28, true); px(0, 29, true);
    px(126, 27, true); px(126, 28, true); px(126, 29, true);
    int done = 125 * st->ota_pct / 100;
    int gap = done > 8 ? (int)((now / 40) % (unsigned)done) : -10;
    for (int x = 1; x < 1 + done; x++) {
        if (x >= gap && x < gap + 3) continue;
        for (int y = 27; y < 30; y++) if (((x + y) & 3) != 0) px(x, y, true);
    }
}

static void alert(uint32_t now) {
    memset(s_fb, 0, W * H / 8);
    fill(0, 0, 127, 31, true);
    int s = text_w(s_alert_label, 3) <= 120 ? 3 : text_w(s_alert_label, 2) <= 120 ? 2 : 1;
    text((127 - text_w(s_alert_label, s)) / 2, (31 - 7 * s) / 2, s_alert_label, s, false);
    (void)now;
}

// ── task ──
static void display_task(void *arg) {
    (void)arg;
    bool inverted = false, dim = false, dark = false;
    uint32_t last_shift = now_ms(), last_decay = now_ms(), last_linked = now_ms();
    display_status_t st;
    for (;;) {
        uint32_t now = now_ms();
        if (s_stealth != dark) {
            dark = s_stealth;
            cmd1(dark ? 0xAE : 0xAF);
        }
        if (dark) { vTaskDelay(pdMS_TO_TICKS(FRAME_MS)); continue; }

        memset(&st, 0, sizeof(st));
        if (s_fill) s_fill(&st);
        if (st.ble || st.usb) last_linked = now;

        if (now - last_decay >= 1000) {   // activity fades; a new second's slot starts at zero
            last_decay = now;
            for (int i = 0; i < 42; i++) s_act[i] = (uint8_t)(s_act[i] * 3 / 4);
            s_new_slot[(now / 1000 + 1) % 10] = 0;
            s_session_at[(now / 1000) % 10] = st.aps_session;
        }
        if (now - last_shift >= SHIFT_MS) {
            last_shift = now;
            static const int8_t OFF[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
            static int k;
            k = (k + 1) % 4;
            s_ox = OFF[k][0]; s_oy = OFF[k][1];
        }

        bool alerting = !st.ota && (int32_t)(s_alert_until - now) > 0;   // an update's screen outranks alerts
        if (st.ota) {
            if (inverted) { inverted = false; cmd1(0xA6); }
            update_screen(&st, now);
        } else if (alerting) {
            alert(now);
            bool inv = ((now - s_alert_since) / ALERT_BLINK_MS) & 1;   // starts with the LED alert, flips in step
            if (inv != inverted) { inverted = inv; cmd1(inv ? 0xA7 : 0xA6); }
        } else {
            if (inverted) { inverted = false; cmd1(0xA6); }
            if (now - s_t0 < 2000) splash(now - s_t0);
            else dashboard(&st, now);
        }
        bool want_dim = !alerting && !st.ota && now - last_linked > IDLE_DIM_MS;
        if (want_dim != dim) {
            dim = want_dim;
            const uint8_t c[] = { 0x81, dim ? CONTRAST_DIM : CONTRAST_ON };
            cmds(c, sizeof(c));
        }
        flush();
        vTaskDelay(pdMS_TO_TICKS(alerting ? ALERT_BLINK_MS / 2 : FRAME_MS));
    }
}

// ── public ──
static bool pulled_up(int sda, int scl) {
    gpio_config_t io = { .pin_bit_mask = (1ULL << sda) | (1ULL << scl), .mode = GPIO_MODE_INPUT,
                         .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&io);
    esp_rom_delay_us(50);
    bool up = gpio_get_level(sda) && gpio_get_level(scl);
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    return up;
}

bool display_probe(int sda, int scl) {
    if (s_bus || !pulled_up(sda, scl)) return false;
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = sda, .scl_io_num = scl,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7 };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) { s_bus = NULL; return false; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000 };
    uint8_t status = 0xFF;
    bool ok = i2c_master_probe(s_bus, ADDR, 50) == ESP_OK && i2c_master_bus_add_device(s_bus, &dc, &s_dev) == ESP_OK &&
              i2c_master_receive(s_dev, &status, 1, 50) == ESP_OK;
    // SSD1306 status byte: bit 6 = display off, bits 0-5 a small controller id; 0xFF is a floating or non-SSD1306 read.
    ok = ok && status != 0xFF && (status & 0x3F) < 0x10;
    ESP_LOGI(TAG, "display probe on SDA %d / SCL %d: %s (status 0x%02x)", sda, scl, ok ? "SSD1306" : "none", status);
    if (!ok) display_release();
    return ok;
}

void display_release(void) {
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
}

void display_start(const char *board, const char *version, display_fill_fn fill_fn) {
    if (!s_dev || s_task) return;
    s_fb = calloc(1, W * H / 8);
    s_sent = malloc(W * H / 8);
    s_seen = calloc(SEEN_N, sizeof(uint32_t));
    if (!s_fb || !s_sent || !s_seen) {
        free(s_fb); free(s_sent); free(s_seen); s_fb = s_sent = NULL; s_seen = NULL;
        ESP_LOGW(TAG, "display: no memory");
        return;
    }
    memset(s_sent, 0xFF, W * H / 8);   // forces the first full frame
    s_board = board; s_version = version; s_fill = fill_fn;
    // SSD1306 128x32 (multiplex 32, sequential COM pins), page addressing, mounted upside down (segment remap + COM
    // scan reversed), charge pump on.
    static const uint8_t INIT[] = { 0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8,
                                    0xDA, 0x02, 0x81, CONTRAST_ON, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF };
    if (cmds(INIT, sizeof(INIT)) != ESP_OK) { ESP_LOGW(TAG, "display didn't take its setup"); return; }
    s_t0 = now_ms();
    if (xTaskCreate(display_task, "wd_disp", 3072, NULL, 1, &s_task) != pdPASS) { s_task = NULL; return; }
    ESP_LOGI(TAG, "display: 128x32 SSD1306 running");
}

bool display_running(void) { return s_task != NULL; }

void display_alert(const char *label, uint32_t ms) {
    if (!s_task) return;
    if (!ms) { s_alert_until = now_ms(); return; }   // `led off`: back to the dashboard
    size_t n = 0;
    for (; label[n] && n < sizeof(s_alert_label) - 1; n++)   // one word from the app: '_' stands for a space
        s_alert_label[n] = label[n] == '_' ? ' ' : (char)toupper((unsigned char)label[n]);
    s_alert_label[n] = '\0';
    if (!n) strcpy(s_alert_label, "ALERT");
    s_alert_since = now_ms();
    s_alert_until = s_alert_since + ms;
}

void display_set_stealth(bool on) { s_stealth = on; }

void display_dump(void (*emit)(int page, int x, const char *hex)) {
    if (!s_sent) return;
    char hex[W + 1];   // half a page per line: 64 column bytes
    for (int page = 0; page < H / 8; page++)
        for (int x0 = 0; x0 < W; x0 += W / 2) {
            for (int x = 0; x < W / 2; x++) snprintf(hex + 2 * x, 3, "%02x", s_sent[page * W + x0 + x]);
            emit(page, x0, hex);
        }
}
