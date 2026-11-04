// Host benchmark of the watch's drawing code: paints each watch scene 400 times through lcd_draw_strip and prints the
// time per frame. With -pg and gprof it shows where painting goes (docs/memory-soak-results.md, rendering).
//   cc -Og -DLCD_WATCH=1 -I main tools/lcd_bench.c main/lcd_draw.c -o /tmp/lcd_bench && /tmp/lcd_bench
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lcd_draw.h"

static void net(lcd_status_t *st, uint32_t id, const char *ssid, int rssi, uint8_t ch, const char *auth) {
    netlog_rec_t *r = &st->nets[st->nets_n++];
    memset(r, 0, sizeof(*r));
    r->bssid[5] = (uint8_t)id;
    r->ssid_len = (uint8_t)strlen(ssid);
    memcpy(r->ssid, ssid, r->ssid_len);
    r->rssi = (int8_t)rssi;
    r->ch = ch;
    r->auth = auth;
}

static double paint(lcd_frame_t *f, int n) {
    static uint16_t strip[LCD_W * LCD_SH];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int k = 0; k < n; k++)
        for (int y0 = 0; y0 < f->h; y0 += LCD_SH) lcd_draw_strip(strip, y0, f);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / n;
}

int main(void) {
    static lcd_frame_t fr;
    lcd_frame_t *f = &fr;
    f->w = 410; f->h = 502; f->t0 = 1000; f->now = 1000 + 1543000; f->bright_pct = 100; f->sleep_s = 30; f->hold_btn = -1;
    f->board = "Waveshare AMOLED 2.06"; f->version = "1.7.6";
    lcd_status_t *st = &f->st;
    st->ble = true; strcpy(st->holder, "Pixel11Pro"); strcpy(st->mode, "WIFI"); strcpy(st->ble_name, "WardriveESP-476A");
    st->mtu = 247; st->channel = 6; f->cur = lcd_act_index(6); st->scanning = st->can_scan = true;
    st->batt_pct = 82; st->batt_mv = 3987; f->live = 23; f->session = 187; f->fresh = 3; f->run = 64;
    st->wall_ok = true; st->wall_hour = 13; st->wall_min = 34; st->wall_wday = 2; st->wall_mday = 3; st->wall_mon = 11;
    static const uint8_t ACT[LCD_ACT_N] = { 200, 10, 30, 0, 0, 240, 60, 0, 20, 0, 220, 40, 0 };
    memcpy(f->act, ACT, sizeof(ACT));
    net(st, 1, "dragino-3296cc", -67, 11, "WPA2"); net(st, 2, "RT-AC66U_B1_A8_2G", -78, 11, "WPA2");
    net(st, 3, "", -81, 6, "WPA2"); net(st, 4, "Starbucks WiFi", -54, 1, "OPEN"); net(st, 5, "VIAANIX_LOWER_2G", -79, 1, "WPA2_WPA3");
    const int N = 400;
    static const struct { int page; const char *name; } P[] = { { LCD_PAGE_CLOCK, "clock" }, { LCD_PAGE_DASH, "dashboard" },
                                                                  { LCD_PAGE_NETS, "networks" }, { LCD_PAGE_CTRL, "controls" } };
    for (size_t i = 0; i < sizeof(P) / sizeof(*P); i++) { f->page = (uint8_t)P[i].page; printf("%-10s %.3f ms/frame\n", P[i].name, paint(f, N)); }
    f->page = LCD_PAGE_DASH; f->alerting = true; f->alert_on = true; f->alert_rgb = 0x00C8FF; strcpy(f->alert, "DRONE");
    printf("%-10s %.3f ms/frame\n", "alert", paint(f, N));
    return 0;
}
