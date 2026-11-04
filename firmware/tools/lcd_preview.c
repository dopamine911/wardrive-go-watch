// Renders the color screens' scenes on a PC with the firmware's own drawing code (main/lcd_draw.c), as PPM images
// (each scene at 240x320 and 128x128, and with -DLCD_WATCH=1 the watch's 410x502 pages), so the layout can be checked
// without flashing a board.
//   cc -O2 -DLCD_WATCH=1 -I main tools/lcd_preview.c main/lcd_draw.c -o lcd_preview && ./lcd_preview <out dir>
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>
#include "lcd_draw.h"

static void save1(const char *dir, const char *name, const lcd_frame_t *f) {
    static uint16_t strip[LCD_W * LCD_SH];
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%dx%d.ppm", dir, name, f->w, f->h);
    FILE *o = fopen(path, "wb");
    if (!o) { perror(path); return; }
    fprintf(o, "P6\n%d %d\n255\n", f->w, f->h);
    for (int y0 = 0; y0 < f->h; y0 += LCD_SH) {
        int rows = f->h - y0 < LCD_SH ? f->h - y0 : LCD_SH;   // the last strip may be partial
        lcd_draw_strip(strip, y0, f);
        for (int i = 0; i < f->w * rows; i++) {
            uint16_t c = (uint16_t)((strip[i] >> 8) | (strip[i] << 8));
            unsigned char px[3] = { (unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                    (unsigned char)((c & 31) * 255 / 31) };
            fwrite(px, 1, 3, o);
        }
    }
    fclose(o);
    printf("%s\n", path);
}

// Each scene on both panels.
static void save(const char *dir, const char *name, lcd_frame_t *f) {
    f->w = 240; f->h = 320; save1(dir, name, f);
    f->w = 128; f->h = 128; save1(dir, name, f);
}

#if LCD_WATCH
static void save_watch(const char *dir, const char *name, lcd_frame_t *f) {
    f->w = 410; f->h = 502; save1(dir, name, f);
}

static void net(lcd_status_t *st, uint32_t id, const char *ssid, int rssi, uint8_t ch, const char *auth) {
    netlog_rec_t *r = &st->nets[st->nets_n++];
    const uint8_t m[6] = { 0xA8, 0x40, 0x41, (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    memcpy(r->bssid, m, 6);
    r->ssid_len = (uint8_t)strlen(ssid);
    strcpy(r->ssid, ssid);
    r->rssi = (int8_t)rssi;
    r->ch = ch;
    r->auth = auth;
}

// The watch's pages with a plausible walk around town.
static void watch_scenes(const char *dir, lcd_frame_t *f) {
    memset(f, 0, sizeof(*f));
    f->board = "Waveshare AMOLED 2.06";
    f->version = "1.7.5";
    f->t0 = 1000;
    f->bright_pct = 100;
    f->sleep_s = 30;
    f->hold_btn = -1;
    static const uint8_t ACT[LCD_ACT_N] = { 200, 10, 30, 0, 0, 240, 60, 0, 20, 0, 220, 40, 0 };
    memcpy(f->act, ACT, sizeof(ACT));
    f->now = f->t0 + 900;
    save_watch(dir, "w1-splash", f);

    lcd_status_t *st = &f->st;
    f->now = f->t0 + 1543000;
    st->ble = true;
    strcpy(st->holder, "Pixel11Pro");
    strcpy(st->mode, "WIFI");
    strcpy(st->ble_name, "WardriveESP-476A");
    st->mtu = 247;
    st->channel = 6;
    f->cur = lcd_act_index(6);
    st->scanning = st->can_scan = true;
    st->batt_pct = 82;
    st->batt_mv = 3987;
    f->live = 23;
    f->session = 187;
    f->fresh = 3;
    f->run = 64;
    f->page = LCD_PAGE_DASH;
    save_watch(dir, "w2-dashboard", f);

    // The clock page: a plain watch face (local time Tue 3 Nov 2026, 1:34 PM), with the mode's counter
    f->page = LCD_PAGE_CLOCK;
    st->wall_ok = true;
    st->wall_hour = 13; st->wall_min = 34; st->wall_wday = 2; st->wall_mday = 3; st->wall_mon = 11; st->wall_year = 2026;
    save_watch(dir, "w14-clock", f);
    st->wall_hour = 0; st->wall_min = 5; st->wall_mday = 21; st->wall_mon = 12; st->wall_wday = 1;
    strcpy(st->mode, "BT"); st->bt_devices = 37; st->batt_pct = 12;
    save_watch(dir, "w15-clock-bt-midnight", f);
    st->wall_ok = false; st->scanning = false; strcpy(st->mode, "WIFI"); st->batt_pct = 82;
    save_watch(dir, "w16-clock-no-time", f);
    st->scanning = true;

    // On USB power: charging (green bolt, green percent), then full and resting (dim bolt)
    st->wall_ok = true; st->wall_hour = 13; st->wall_min = 34; st->wall_wday = 2; st->wall_mday = 3; st->wall_mon = 11;
    st->charge = LCD_CHARGE_ON; st->batt_pct = 64;
    save_watch(dir, "w17-clock-charging", f);
    f->page = LCD_PAGE_DASH;
    save_watch(dir, "w18-dashboard-charging", f);
    st->charge = LCD_CHARGE_PLUGGED; st->batt_pct = 100;
    save_watch(dir, "w19-dashboard-usb-full", f);
    f->page = LCD_PAGE_CLOCK;
    save_watch(dir, "w20-clock-usb-full", f);
    st->charge = LCD_CHARGE_NONE; st->batt_pct = 82;

    net(st, 1, "dragino-3296cc", -67, 11, "WPA2");
    net(st, 2, "RT-AC66U_B1_A8_2G", -78, 11, "WPA2");
    net(st, 3, "", -81, 6, "WPA2");
    net(st, 4, "Starbucks WiFi", -54, 1, "OPEN");
    net(st, 5, "VIAANIX_LOWER_2G", -79, 1, "WPA2_WPA3");
    net(st, 6, "caf\xc3\xa9-guest", -71, 6, "WPA");
    net(st, 7, "Viaanix", -48, 7, "WPA2");
    net(st, 8, "a-very-long-network-name-indeed-1", -88, 13, "WEP");
    f->page = LCD_PAGE_NETS;
    save_watch(dir, "w3-networks", f);
    f->net_first = 5;
    save_watch(dir, "w4-networks-scrolled", f);

    f->page = LCD_PAGE_CTRL;
    f->hold_btn = LCD_BTN_BRIGHT;
    f->hold_pct = 60;
    save_watch(dir, "w5-controls-holding", f);

    f->hold_btn = -1;
    f->hold_pct = 0;
    st->ble = false;
    st->holder[0] = 0;
    st->scanning = st->can_scan = false;
    st->mtu = 0;
    st->channel = 0;
    f->cur = -1;
    f->live = 0;
    f->fresh = 0;
    save_watch(dir, "w6-controls-no-phone", f);
    f->page = LCD_PAGE_DASH;
    save_watch(dir, "w7-dashboard-no-phone", f);
    st->nets_n = 0;
    f->page = LCD_PAGE_NETS;
    save_watch(dir, "w8-networks-empty", f);

    // Bluetooth mode: the device count and the devices page; capture mode: its artifacts.
    st->ble = true;
    strcpy(st->holder, "Pixel11Pro");
    st->scanning = st->can_scan = true;
    strcpy(st->mode, "BT");
    st->bt_devices = 37;
    st->bt_now = 12;
    st->bt_windows = 9;
    st->nets_n = 0;
    net(st, 0x101, "Galaxy Buds2", -52, 0, "public");
    net(st, 0x102, "", -77, 0, "random");
    net(st, 0x103, "Tile", -69, 0, "random");
    net(st, 0x104, "JBL Flip 6", -84, 0, "public");
    f->page = LCD_PAGE_DASH;
    save_watch(dir, "w11-dashboard-bt", f);
    f->page = LCD_PAGE_NETS;
    save_watch(dir, "w12-devices-bt", f);
    strcpy(st->mode, "CAP");
    st->channel = 6;
    f->cur = lcd_act_index(6);
    f->live = 19;
    st->cap_hs = 2;
    st->cap_pmkid = 1;
    st->cap_clients = 14;
    f->page = LCD_PAGE_DASH;
    save_watch(dir, "w13-dashboard-cap", f);
    strcpy(st->mode, "WIFI");
    st->nets_n = 0;

    f->page = LCD_PAGE_DASH;
    f->alerting = true;
    f->alert_on = true;
    f->alert_rgb = 0x00C8FF;
    strcpy(f->alert, "DRONE");
    save_watch(dir, "w9-alert", f);
    f->alerting = false;
    st->ota = true;
    st->ota_pct = 63;
    st->ota_total_kb = 1192;
    st->ota_kb = 751;
    save_watch(dir, "w10-update", f);
}
#endif

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    lcd_frame_t f;
    memset(&f, 0, sizeof(f));
    f.board = "Koko Marauder V8";
    f.version = "1.6.9";
    f.t0 = 1000;
    f.st.batt_pct = 87;
    static const uint8_t ACT[LCD_ACT_N] = { 120, 10, 30, 0, 0, 240, 60, 0, 20, 0, 200, 40, 0, 0,
                                           180, 90, 0, 140, 0, 0, 60, 0, 0, 0, 30, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 220, 70, 150, 0, 30, 0, 0, 0 };
    memcpy(f.act, ACT, sizeof(ACT));

    f.now = f.t0 + 700;
    save(dir, "1-splash", &f);

    f.now = f.t0 + 754000;
    f.st.usb = true;
    strcpy(f.st.holder, "Jeremy's OnePlus");
    strcpy(f.st.mode, "WIFI");
    f.st.channel = 36;
    f.cur = lcd_act_index(36);
    f.st.captures = 3;
    f.st.bt_devices = 41;
    f.live = 187;
    f.session = 1287;
    f.run = 412;
    f.fresh = 12;
    save(dir, "2-dashboard", &f);

    f.st.usb = false;
    f.st.holder[0] = 0;
    strcpy(f.st.mode, "CSI");
    f.st.channel = 6;
    f.cur = lcd_act_index(6);
    f.live = 1934;
    f.session = 248913;
    f.fresh = 0;
    f.st.captures = 0;
    f.st.batt_pct = 12;
    save(dir, "3-dashboard-idle", &f);
    f.st.sd_log = true;   // no phone, saving to the card
    f.st.sd_rows = 38412;
    f.st.sd_waiting = 3;
    f.run = 2871;
    strcpy(f.st.mode, "WIFI");
    f.live = 143;
    f.session = 2871;
    f.fresh = 9;
    save(dir, "3b-dashboard-saving", &f);
    f.st.sd_log = false;
    f.st.sd_rows = 0;

    f.alerting = true;
    f.alert_on = true;
    strcpy(f.alert, "FLOCK CAMERA");
    save(dir, "4-alert-on", &f);
    f.alert_on = false;
    save(dir, "5-alert-off", &f);
    strcpy(f.alert, "ADULT TOY");
    save(dir, "6-alert-toy", &f);
    f.now += 900;
    save(dir, "7-alert-toy-later", &f);

    f.alerting = false;
    f.st.ota = true;
    f.st.ota_pct = 63;
    f.st.ota_total_kb = 1601;
    f.st.ota_kb = 1009;
    save(dir, "8-update", &f);
    f.st.ota_pct = 100;
    f.st.ota_kb = 1601;
    f.st.ota_installing = true;
    save(dir, "9-update-installing", &f);

    // offline runs going to the phone
    memset(&f.st, 0, sizeof(f.st));
    f.st.batt_pct = 87;
    f.st.ble = true;
    f.st.sd_send = true;
    f.st.sd_run = 2;
    f.st.sd_runs = 3;
    strcpy(f.st.sd_file, "run_12.csv");
    f.st.sd_size = 1280 * 1024;
    f.st.sd_bytes = 412 * 1024;
    f.st.sd_rate = 6246;
    save(dir, "10-sd-sending", &f);
    f.now += 130;
    save(dir, "11-sd-sending-later", &f);
    f.st.sd_run = 3;
    strcpy(f.st.sd_file, "run_13.csv");
    f.st.sd_size = 38 * 1024 + 300;
    f.st.sd_bytes = f.st.sd_size;
    save(dir, "12-sd-saving", &f);
    f.st.sd_send = false;
    f.st.sd_sent = true;
    f.st.sd_sent_ms = 1200;
    f.st.sd_sent_bytes = 2391 * 1024;
    save(dir, "13-sd-sent", &f);
#if LCD_WATCH
    watch_scenes(dir, &f);
#endif
    return 0;
}
