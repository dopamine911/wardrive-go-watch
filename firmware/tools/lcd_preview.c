// Renders the color screens' scenes on a PC with the firmware's own drawing code (main/lcd_draw.c), as PPM images
// (each scene at 240x320 and 128x128), so the layout can be checked without flashing a board.
//   zig cc -O2 -I main tools/lcd_preview.c main/lcd_draw.c -o lcd_preview && ./lcd_preview <out dir>
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
        lcd_draw_strip(strip, y0, f);
        for (int i = 0; i < f->w * LCD_SH; i++) {
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
    return 0;
}
